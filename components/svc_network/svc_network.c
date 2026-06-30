#include "svc_network.h"
#include <string.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"

// Khai báo extern để tránh lỗi Circular Dependency trong CMake (svc_cloud <->
// svc_network)
extern void svc_cloud_flush_cache_to_nvs(void);
#include "esp_modem_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "drv_a7680c.h"
#include "sys_manager.h"
#include "nvs.h"

static const char *TAG = "SVC_NETWORK";
/// Không giới hạn số lần retry kết nối để đảm bảo tính sẵn sàng của thiết bị
/// (xem wifi_event_handler).
// #define MAXIMUM_RETRY 5  // Hiện không dùng: retry không giới hạn (xem
// wifi_event_handler). Giữ lại để tham khảo nếu sau này cần giới hạn số lần
// thử.

static volatile bool s_connected = false;
static int s_retry_num = 0;

static esp_modem_dce_t *s_dce = NULL;
static esp_netif_t *s_ppp_netif = NULL;
static int s_last_4g_rssi = 0;

/// Chu kỳ đo RSSI 4G (ms). 0 = tắt hẳn. Mặc định 300s (mỗi lần đo ngắt PPP ~15-20s).
static volatile uint32_t s_rssi_interval_ms = 300000;

static void delayed_restart_task(void *arg)
{
    ESP_LOGE(TAG,
             "Mạng hoàn toàn thất bại. Đi ngủ 60 giây trước khi thử lại...");
    vTaskDelay(pdMS_TO_TICKS(60000));

    // Ghi các cảnh báo chưa gửi được vào NVS trước khi reset
    svc_cloud_flush_cache_to_nvs();

    ESP_LOGI(TAG, "Khởi động lại hệ thống (Restarting)...");
    esp_restart();
}

/**
 * @brief Task ngầm chạy mỗi 60 giây để thoát tạm thời chế độ PPP,
 * đo cường độ sóng 4G, rồi nối lại PPP. Cập nhật biến s_last_4g_rssi.
 */
static void cellular_rssi_update_task(void *arg)
{
    while (1)
    {
        /// Nếu rssi_interval = 0 (tắt), ngủ 60s rồi tiếp tục vòng lặp mà không đo.
        /// Ngược lại ngủ đúng chu kỳ đã cấu hình (mặc định 300s).
        uint32_t interval = s_rssi_interval_ms;
        if (interval == 0) {
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(interval));

        if (s_connected && s_dce != NULL)
        {
            if (sys_manager_get_state() != STATE_NORMAL || sys_manager_is_comms_critical()) {
                ESP_LOGD(TAG, "Skip RSSI: state hoặc đang trong cửa sổ confirm/alert");
                continue;
            }

            ESP_LOGI(TAG, "Tạm ngắt PPP (+++) để đo sóng 4G...");

            // 1. Chuyển sang COMMAND mode (gửi mã escape +++)
            if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND) == ESP_OK)
            {
                // 2. Đo RSSI bằng AT+CSQ với timeout cứng 5 giây
                char at_out[32] = {0};
                if (esp_modem_at(s_dce, "AT+CSQ", at_out, 5000) == ESP_OK)
                {
                    // Cắt chuỗi "+CSQ: <rssi>,<ber>"
                    int rssi = 0;
                    if (sscanf(at_out, "+CSQ: %d", &rssi) == 1)
                    {
                        if (rssi != 99)
                            s_last_4g_rssi = rssi;
                        ESP_LOGI(TAG, "Cập nhật RSSI 4G thành công: CSQ = %d",
                                 s_last_4g_rssi);
                    }
                }
                else
                {
                    ESP_LOGW(
                        TAG,
                        "Đo sóng thất bại (Timeout 5s) trong Command mode");
                }

                // 3. Quay lại DATA mode (gửi lệnh ATO)
                if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA) != ESP_OK)
                {
                    ESP_LOGE(TAG,
                             "Lỗi nghiêm trọng: Không thể quay lại chế độ PPP "
                             "(ATO). Khởi động lại thiết bị!");

                    // Ghi các cảnh báo chưa gửi được vào NVS trước khi reset
                    svc_cloud_flush_cache_to_nvs();

                    esp_restart();
                }
                else
                {
                    ESP_LOGI(TAG, "Đã resume luồng PPP (ATO) an toàn.");
                }
            }
            else
            {
                ESP_LOGW(TAG, "Không thể vào COMMAND mode. Bỏ qua lần đo này.");
            }
        }
    }
}

/**
 * @brief Xử lý các sự kiện WiFi và IP của tầng STA (kết nối, ngắt kết nối, nhận
 * IP).
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
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_connected = false;
        esp_event_post(NET_EVENT, NET_EVT_DISCONNECTED, NULL, 0, portMAX_DELAY);

        if (s_retry_num < 5)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying Wi-Fi connection... (attempt %d)",
                     s_retry_num);
        }
        else
        {
            ESP_LOGE(TAG, "WiFi connection failed after 5 attempts!");
            xTaskCreate(delayed_restart_task, "delayed_restart", 2048, NULL, 5,
                        NULL);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        // Phát event báo kết nối thành công cho sys_manager
        esp_event_post(NET_EVENT, NET_EVT_WIFI_CONNECTED, NULL, 0,
                       portMAX_DELAY);
    }
}

/**
 * @brief Khởi tạo WiFi ở chế độ Station: tạo netif, đăng ký event handler, cấu
 * hình và start.
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
        .sta =
            {
                /// Yêu cầu tối thiểu WPA/WPA2-PSK để tương thích đa số router
                /// gia đình mà vẫn đảm bảo bảo mật
                .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
            },
    };
    strncpy((char *)wifi_config.sta.ssid, ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass,
            sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /// Init non-blocking: hàm trả về ngay, việc kết nối thực tế diễn ra bất
    /// đồng bộ qua event handler
    ESP_LOGI(TAG, "WiFi STA initialized. Connecting to \"%s\"...", ssid);
    return ESP_OK;
}

/**
 * @brief Trả về cờ trạng thái kết nối mạng đang được lưu nội bộ (WiFi hoặc
 * PPP).
 * @return true nếu đang kết nối (đã có IP), ngược lại false.
 */
bool svc_network_is_connected(void)
{
    return s_connected;
}

int svc_network_get_rssi(void)
{
    if (s_connected)
    {
        wifi_ap_record_t ap;
        // Chỉ lấy rssi nếu cấu hình hiện tại đang là WiFi.
        // esp_wifi_sta_get_ap_info sẽ trả về ESP_OK nếu WiFi đang kết nối.
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            return ap.rssi;
        }
        // Nếu không phải WiFi (nghĩa là đang dùng 4G PPP)
        // Vì chế độ DATA không cho phép gửi AT command song song, ta trả về giá
        // trị RSSI lấy được ngay trước khi dial PPP (lưu trong s_last_4g_rssi).
        return s_last_4g_rssi;
    }
    return 0;
}

/**
 * @brief Đặt chu kỳ đo RSSI 4G. 0 = tắt. Giá trị != 0 bị clamp tối thiểu 60000 ms.
 * @param ms Chu kỳ mới (milli-giây).
 */
void svc_network_set_rssi_interval_ms(uint32_t ms)
{
    if (ms != 0 && ms < 60000) {
        ms = 60000;
    }
    s_rssi_interval_ms = ms;
    ESP_LOGI(TAG, "RSSI interval set to %lu ms (%s)", (unsigned long)ms, ms == 0 ? "OFF" : "ON");
}

/**
 * @brief Lấy chu kỳ đo RSSI 4G hiện tại (ms). 0 nếu đang tắt.
 */
uint32_t svc_network_get_rssi_interval_ms(void)
{
    return s_rssi_interval_ms;
}

// ============================================================================
//  ĐƯỜNG MẠNG 4G LTE (PPPoS qua A7680C + esp_modem)
// ============================================================================

/**
 * @brief Xử lý sự kiện IP của netif PPP: phát NET_EVT_CELLULAR_CONNECTED khi có
 * IP, NET_EVT_DISCONNECTED khi mất IP.
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
        esp_event_post(NET_EVENT, NET_EVT_CELLULAR_CONNECTED, NULL, 0,
                       portMAX_DELAY);
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

    /// 1. Reset cứng phần cứng qua chân RST, sau đó chờ module boot (~12s).
    drv_a7680c_init((gpio_num_t)cfg->rst_pin);
    bool did_reset = false;
    drv_a7680c_reset(&did_reset);
    if (did_reset) {
        vTaskDelay(pdMS_TO_TICKS(12000));
    } else {
        vTaskDelay(pdMS_TO_TICKS(500)); // Module không reset, chỉ settle ngắn
    }

    /// 1.5 Sniff UART bus phát hiện module (Hướng A). Đã đo bằng log thực:
    ///   - KHÔNG module → bus im tuyệt đối. CÓ module → phun Rx Break (cold boot)
    ///     hoặc byte PPP (kẹt DATA mode).
    /// BẪY: chân RX thả nổi (không module) dễ sinh GLITCH lẻ → false-positive.
    /// Chống bằng: (1) pull-up giữ RX idle-HIGH, (2) xả event rác lúc install,
    /// (3) yêu cầu hoạt động BỀN (≥ ngưỡng) chứ không tin 1 event lẻ.
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    QueueHandle_t uart_queue;
    ESP_ERROR_CHECK(uart_driver_install(cfg->uart_port, 2048, 2048, 20, &uart_queue, 0));
    uart_param_config(cfg->uart_port, &uart_config);
    uart_set_pin(cfg->uart_port, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    /// Pull-up RX: KHÔNG module → đường idle-HIGH, không sinh break/glitch giả.
    gpio_set_pull_mode((gpio_num_t)cfg->rx_pin, GPIO_PULLUP_ONLY);
    uart_flush_input(cfg->uart_port);
    xQueueReset(uart_queue);

    int activity = 0;       // số event hợp lệ (break/data)
    bool saw_data = false;  // có byte thật → nghi module kẹt DATA mode (cần +++)
    uart_event_t event;
    uint64_t start_time = esp_timer_get_time();
    const int ACTIVITY_THRESH = 5; // module thật phun hàng trăm break → 5 thừa sức, loại glitch lẻ

    /// Cửa sổ 10s: module thật bắt đầu phun break ~6-7s sau power-on (đo từ log) nên
    /// không được cắt ngắn dưới mức đó kẻo false-negative một module thật → rơi WiFi oan.
    ESP_LOGI(TAG, "Đang sniff bus UART (tối đa 10s) để phát hiện module 4G...");
    while ((esp_timer_get_time() - start_time) < 10000000ULL) {
        if (xQueueReceive(uart_queue, (void *)&event, pdMS_TO_TICKS(100))) {
            if (event.type == UART_DATA)       { activity++; saw_data = true; }
            else if (event.type == UART_BREAK) { activity++; }
            if (activity >= ACTIVITY_THRESH) break;
        }
    }

    // Xóa driver tạm để nhường cổng UART cho esp_modem
    uart_driver_delete(cfg->uart_port);

    if (activity < ACTIVITY_THRESH) {
        ESP_LOGW(TAG, "Bus UART im (activity=%d) → KHÔNG có module 4G → fallback WiFi.", activity);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Phát hiện module 4G trên bus (activity=%d, data=%d) → tiếp tục init Cellular.",
             activity, (int)saw_data);

    /// 2. Tạo netif PPP và đăng ký lắng nghe sự kiện IP của nó.
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&netif_cfg);
    if (s_ppp_netif == NULL)
    {
        ESP_LOGE(TAG, "Tạo netif PPP thất bại");
        return ESP_FAIL;
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                               &ppp_ip_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                               &ppp_ip_event_handler, NULL);

    /// 3. Cấu hình DTE (UART nối module) — không dùng flow control phần cứng
    /// (chưa nối RTS/CTS).
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num = cfg->uart_port;
    dte_cfg.uart_config.tx_io_num = cfg->tx_pin;
    dte_cfg.uart_config.rx_io_num = cfg->rx_pin;
    dte_cfg.uart_config.rts_io_num = -1;
    dte_cfg.uart_config.cts_io_num = -1;
    dte_cfg.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_cfg.uart_config.baud_rate = 115200;
    dte_cfg.uart_config.rx_buffer_size  = 16384;  // mặc định 4096
    dte_cfg.uart_config.tx_buffer_size  = 2048;   // mặc định 512
    dte_cfg.uart_config.event_queue_size = 40;
    dte_cfg.task_priority = 9;   // cao gần imu_task(10) để không bị bỏ đói khi crunch IMU+AI
    dte_cfg.dte_buffer_size = 1024;

    /// 4. Cấu hình DCE với APN. A7680C dùng tập lệnh AT tương thích SIM7600 nên
    ///    chọn device type ESP_MODEM_DCE_SIM7600 của esp_modem.
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(cfg->apn);
    /// Bật log DEBUG cho esp_modem để xem được toàn bộ mã HEX truyền nhận UART
    /// (rất hữu ích để fix lỗi chéo dây hoặc sai baudrate).
    esp_log_level_set("esp-modem", ESP_LOG_DEBUG);
    esp_log_level_set("esp_modem", ESP_LOG_DEBUG);
    
    /// Dập spam Rx Break lúc boot module (từ lớp dưới của uart terminal)
    esp_log_level_set("uart_terminal", ESP_LOG_ERROR);

    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_cfg, &dce_cfg,
                              s_ppp_netif);
    if (s_dce == NULL)
    {
        ESP_LOGE(TAG, "Tạo esp_modem DCE thất bại");
        return ESP_FAIL;
    }

    char at_out[64] = {0};
    esp_err_t err;

    /// 5. Đồng bộ Autobaud bằng hàm chuẩn của thư viện esp_modem (gửi chuỗi AT
    /// liên tục đúng chuẩn).
    /// Sniff đã xác nhận CÓ module nên KIÊN NHẪN chờ boot (cold boot A7680C ~25-31s),
    /// KHÔNG bỏ cuộc sớm. +++ (thoát DATA mode) CHỈ gửi khi sniff thấy BYTE PPP
    /// (saw_data) — nghi kẹt DATA mode sau MCU warm-reboot. Cold boot (chỉ Rx Break)
    /// KHÔNG +++ vì gửi lúc module chưa boot xong sẽ chờ ACK vô ích ~25s (đúng thủ
    /// phạm 40s trong log trước).
    bool modem_responded = false;
    if (saw_data) {
        ESP_LOGW(TAG, "Sniff thấy byte PPP → nghi kẹt DATA mode, gửi +++ thoát trước khi sync...");
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    for (int i = 0; i < 35; i++)
    {
        if (esp_modem_sync(s_dce) == ESP_OK)
        {
            modem_responded = true;
            break;
        }
        if ((i % 5) == 0)
            ESP_LOGW(TAG, "Chờ module boot & phản hồi AT (sync)... (%d/35)", i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!modem_responded)
    {
        ESP_LOGE(TAG,
                 "Module 4G không phản hồi qua UART. Hủy khởi tạo Cellular.");
        if (s_dce) {
            esp_modem_destroy(s_dce);
            s_dce = NULL;
        }
        if (s_ppp_netif) {
            esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP, &ppp_ip_event_handler);
            esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_LOST_IP, &ppp_ip_event_handler);
            esp_netif_destroy(s_ppp_netif);
            s_ppp_netif = NULL;
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Phát hiện module 4G trên UART → dùng Cellular");

    /// Khóa cứng Baudrate ở 115200 trên module (tránh tình trạng Auto-baud bị
    /// trôi mất nhịp khi vào CMUX)
    esp_modem_at(s_dce, "AT+IPR=115200", at_out, 2000);

    /// 6a. Kiểm tra SIM sẵn sàng (chẩn đoán). Kỳ vọng "+CPIN: READY".
    if (esp_modem_at(s_dce, "AT+CPIN?", at_out, 2000) == ESP_OK)
        ESP_LOGI(TAG, "SIM status: %s", at_out);

    /// 6b. KHÓA LTE-ONLY (AT+CNMP=38, AUTO_SAVE) — chỉ cần làm LẦN ĐẦU.
    /// Module tự nhớ trong NVRAM nên các boot sau bỏ qua CFUN cycle + chờ 10s → tiết kiệm ~15-25s.
    uint8_t lte_locked = 0;
    nvs_handle_t nh;
    if (nvs_open("config", NVS_READONLY, &nh) == ESP_OK) {
        nvs_get_u8(nh, "lte_lock", &lte_locked);
        nvs_close(nh);
    }

    if (!lte_locked) {
        // LẦN ĐẦU TỚI: Làm full chu trình tắt bật sóng để module nhận chuẩn
        esp_modem_at(s_dce, "AT+CFUN=0", at_out, 5000);
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_modem_at(s_dce, "AT+CNMP=38", at_out, 3000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AT+CNMP=38 (LTE only) thất bại (err=%d)", err);
        } else {
            ESP_LOGI(TAG, "Đã khóa LTE-only (CNMP=38, full cycle)");
            if (nvs_open("config", NVS_READWRITE, &nh) == ESP_OK) {
                nvs_set_u8(nh, "lte_lock", 1);
                nvs_commit(nh);
                nvs_close(nh);
            }
        }
        esp_modem_at(s_dce, "AT+CFUN=1", at_out, 10000);
        ESP_LOGI(TAG, "Đang chờ dọn dẹp UART URC và dò mạng LTE (10s)...");
        vTaskDelay(pdMS_TO_TICKS(10000));
    } else {
        // CÁC LẦN BOOT SAU: Module không lưu CNMP=38 qua power cycle, nên ta GỬI LẠI
        // nhưng BỎ QUA chu trình tắt/bật RF (CFUN) và bỏ qua 10s delay.
        esp_modem_at(s_dce, "AT+CNMP=38", at_out, 3000);
        ESP_LOGI(TAG, "Đã khóa lại LTE-only (Fast-path). Đang chờ attach mạng...");
        
        // Polling CGATT để vào mạng nhanh nhất có thể thay vì fix cứng 10s
        bool attached = false;
        for (int i = 0; i < 15; i++) {
            if (esp_modem_at(s_dce, "AT+CGATT?", at_out, 2000) == ESP_OK && strstr(at_out, "+CGATT: 1")) {
                attached = true;
                ESP_LOGI(TAG, "LTE attach thành công (mất %d giây)", i + 1);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!attached) {
            ESP_LOGW(TAG, "Timeout chờ LTE attach, vẫn thử dial PPP...");
        }
    }

    /// 6c. Lưu lại RSSI ngay trước khi vào chế độ DATA
    int rssi = 0, ber = 0;
    if (esp_modem_get_signal_quality(s_dce, &rssi, &ber) == ESP_OK)
    {
        if (rssi != 99)
            s_last_4g_rssi = rssi;
        ESP_LOGI(TAG, "Signal Quality (RSSI): %d", rssi);
    }

    /// 7. Chuyển sang chế độ DATA → esp_modem tự đặt PDP context (APN) và dial
    /// PPP.
    ///    Lưu ý: Không dùng CMUX vì thiếu phần cứng Flow Control (RTS/CTS),
    ///    module sẽ gây lỗi Checksum (FCS_ERR / reason 3) khiến toàn bộ mạng bị
    ///    sập.
    err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Chuyển PPP DATA mode thất bại (err=%d)", err);
        return err;
    }

    ESP_LOGI(TAG, "Modem đang dial PPP, chờ cấp IP...");

    /// 8. Tạo task ngầm chuyên biệt để đo sóng 4G mỗi phút.
    /// Priority 1 (thấp nhất) để không chiếm CPU của TinyML và IMU (Priority
    /// 5+).
    xTaskCreate(cellular_rssi_update_task, "4g_rssi_task", 3072, NULL, 1, NULL);

    return ESP_OK;
}
