#include "svc_network.h"
#include <string.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "drv_a7680c.h"
#include "sys_manager.h"

static const char *TAG = "SVC_NETWORK";
/// Không giới hạn số lần retry kết nối để đảm bảo tính sẵn sàng của thiết bị (xem wifi_event_handler).
// #define MAXIMUM_RETRY 5  // Hiện không dùng: retry không giới hạn (xem wifi_event_handler).
                            // Giữ lại để tham khảo nếu sau này cần giới hạn số lần thử.

static volatile bool s_connected = false;
static int s_retry_num = 0;

/**
 * @brief Xử lý các sự kiện WiFi và IP của tầng STA (kết nối, ngắt kết nối, nhận IP).
 * @param arg Con trỏ tham số người dùng (không sử dụng).
 * @param event_base Nhóm sự kiện (WIFI_EVENT hoặc IP_EVENT).
 * @param event_id Mã định danh sự kiện cụ thể trong nhóm.
 * @param event_data Dữ liệu kèm theo sự kiện (vd: thông tin IP khi nhận được).
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        // Bắt đầu kết nối ngay khi tầng STA khởi động xong
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_connected = false;
        esp_event_post(NET_EVENT, NET_EVT_DISCONNECTED, NULL, 0, portMAX_DELAY);
        /// Không giới hạn số lần thử kết nối để đảm bảo tính sẵn sàng
        esp_wifi_connect();
        s_retry_num++;
        ESP_LOGI(TAG, "Retrying Wi-Fi connection... (attempt %d)", s_retry_num);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        // Phát event báo kết nối thành công cho sys_manager
        esp_event_post(NET_EVENT, NET_EVT_WIFI_CONNECTED, NULL, 0, portMAX_DELAY);
    }
}

/**
 * @brief Khởi tạo WiFi ở chế độ Station: tạo netif, đăng ký event handler, cấu hình và start.
 * @param ssid Tên mạng WiFi (SSID) cần kết nối.
 * @param pass Mật khẩu WiFi tương ứng với SSID.
 * @return ESP_OK nếu khởi tạo thành công.
 */
esp_err_t svc_network_init(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Starting WiFi STA Init...");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Đăng ký handler cho mọi sự kiện WiFi và sự kiện nhận IP
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            /// Yêu cầu tối thiểu WPA/WPA2-PSK để tương thích đa số router gia đình mà vẫn đảm bảo bảo mật
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /// Init non-blocking: hàm trả về ngay, việc kết nối thực tế diễn ra bất đồng bộ qua event handler
    ESP_LOGI(TAG, "WiFi STA initialized. Connecting to \"%s\"...", ssid);
    return ESP_OK;
}

/**
 * @brief Trả về cờ trạng thái kết nối mạng đang được lưu nội bộ (WiFi hoặc PPP).
 * @return true nếu đang kết nối (đã có IP), ngược lại false.
 */
bool svc_network_is_connected(void)
{
    return s_connected;
}

int svc_network_get_rssi(void)
{
    if (s_connected) {
        wifi_ap_record_t ap;
        // Chỉ lấy rssi nếu cấu hình hiện tại đang là WiFi.
        // esp_wifi_sta_get_ap_info sẽ trả về ESP_OK nếu WiFi đang kết nối.
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            return ap.rssi;
        }
    }
    // Nếu dùng 4G LTE (PPPoS data mode) mà chưa có CMUX, ta tạm trả về 0
    return 0;
}

// ============================================================================
//  ĐƯỜNG MẠNG 4G LTE (PPPoS qua A7680C + esp_modem)
// ============================================================================

static esp_modem_dce_t *s_dce = NULL;
static esp_netif_t *s_ppp_netif = NULL;

/**
 * @brief Xử lý sự kiện IP của netif PPP: phát NET_EVT_CELLULAR_CONNECTED khi có IP,
 *        NET_EVT_DISCONNECTED khi mất IP.
 * @param arg Không dùng.
 * @param base Event base (IP_EVENT).
 * @param event_id Mã sự kiện (PPP got/lost IP).
 * @param event_data Dữ liệu sự kiện (ip_event_got_ip_t khi got IP).
 */
static void ppp_ip_event_handler(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "PPP got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        esp_event_post(NET_EVENT, NET_EVT_CELLULAR_CONNECTED, NULL, 0, portMAX_DELAY);
    }
    else if (event_id == IP_EVENT_PPP_LOST_IP)
    {
        ESP_LOGW(TAG, "PPP lost IP");
        s_connected = false;
        esp_event_post(NET_EVENT, NET_EVT_DISCONNECTED, NULL, 0, portMAX_DELAY);
    }
}

esp_err_t svc_network_init_cellular(const svc_network_cellular_cfg_t *cfg)
{
    ESP_LOGI(TAG, "Starting 4G LTE (PPPoS) init, APN=\"%s\"...", cfg->apn);

    /// 1. Bật nguồn module bằng PWRKEY rồi chờ module boot xong UART (~8s).
    drv_a7680c_init((gpio_num_t)cfg->pwrkey_pin);
    drv_a7680c_power_on();
    vTaskDelay(pdMS_TO_TICKS(8000));

    /// 2. Tạo netif PPP và đăng ký lắng nghe sự kiện IP của nó.
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&netif_cfg);
    if (s_ppp_netif == NULL)
    {
        ESP_LOGE(TAG, "Tạo netif PPP thất bại");
        return ESP_FAIL;
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, &ppp_ip_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, &ppp_ip_event_handler, NULL);

    /// 3. Cấu hình DTE (UART nối module) — không dùng flow control phần cứng (chưa nối RTS/CTS).
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num = cfg->uart_port;
    dte_cfg.uart_config.tx_io_num = cfg->tx_pin;
    dte_cfg.uart_config.rx_io_num = cfg->rx_pin;
    dte_cfg.uart_config.rts_io_num = -1;
    dte_cfg.uart_config.cts_io_num = -1;
    dte_cfg.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_cfg.uart_config.baud_rate = 115200;

    /// 4. Cấu hình DCE với APN. A7680C dùng tập lệnh AT tương thích SIM7600 nên
    ///    chọn device type ESP_MODEM_DCE_SIM7600 của esp_modem.
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(cfg->apn);
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_cfg, &dce_cfg, s_ppp_netif);
    if (s_dce == NULL)
    {
        ESP_LOGE(TAG, "Tạo esp_modem DCE thất bại");
        return ESP_FAIL;
    }

    char at_out[64] = {0};
    esp_err_t err;

    /// 5. Đồng bộ AT (module có thể còn đang boot, thử vài lần).
    for (int i = 0; i < 5; i++)
    {
        if (esp_modem_at(s_dce, "AT", at_out, 2000) == ESP_OK) break;
        ESP_LOGW(TAG, "Chờ module phản hồi AT... (%d)", i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /// 6a. Kiểm tra SIM sẵn sàng (chẩn đoán). Kỳ vọng "+CPIN: READY".
    if (esp_modem_at(s_dce, "AT+CPIN?", at_out, 2000) == ESP_OK)
        ESP_LOGI(TAG, "SIM status: %s", at_out);

    /// 6b. KHÓA LTE-ONLY. AT+CNMP=38 là AUTO_SAVE (lưu NVRAM) → chỉ cần set một lần,
    ///     từ lần boot SAU module vào thẳng LTE, KHÔNG quét 2G/3G → triệt tiêu xung
    ///     dòng tức thời (đúng mục tiêu phần cứng). Theo best-practice của datasheet:
    ///     tắt RF (CFUN=0) → đổi chế độ mạng → bật lại RF (CFUN=1) để baseband
    ///     scan/attach lại sạch trong đúng chế độ LTE-only.
    esp_modem_at(s_dce, "AT+CFUN=0", at_out, 5000);
    vTaskDelay(pdMS_TO_TICKS(200));
    err = esp_modem_at(s_dce, "AT+CNMP=38", at_out, 3000);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "AT+CNMP=38 (LTE only) thất bại (err=%d)", err);
    else
        ESP_LOGI(TAG, "Đã khóa LTE-only (CNMP=38, AUTO_SAVE)");
    esp_modem_at(s_dce, "AT+CFUN=1", at_out, 10000);
    vTaskDelay(pdMS_TO_TICKS(3000));  // chờ RF bật lại + attach mạng LTE

    /// 6c. (chẩn đoán) cường độ sóng — "+CSQ: <rssi>,<ber>", rssi 99 = chưa có sóng.
    if (esp_modem_at(s_dce, "AT+CSQ", at_out, 2000) == ESP_OK)
        ESP_LOGI(TAG, "Signal: %s", at_out);

    /// 7. Chuyển sang chế độ DATA → esp_modem tự đặt PDP context (APN) và dial PPP.
    ///    Khi PPP lên, IP_EVENT_PPP_GOT_IP sẽ kích hoạt ppp_ip_event_handler().
    err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Chuyển PPP DATA mode thất bại (err=%d)", err);
        return err;
    }

    ESP_LOGI(TAG, "Modem đang dial PPP, chờ cấp IP...");
    return ESP_OK;
}
