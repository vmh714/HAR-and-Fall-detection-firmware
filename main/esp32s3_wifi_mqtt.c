#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

// Định nghĩa thông tin Wi-Fi của bạn
#define WIFI_SSID "MD_LAPTOP"
#define WIFI_PASS "11111111"
#define MAXIMUM_RETRY 5

// --- THÊM MỚI: Khai báo Event Group và Bit cờ báo hiệu ---
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "wifi_station";
static int s_retry_num = 0;

// Hàm xử lý sự kiện Wi-Fi / IP
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Thử kết nối lại vào AP...");
        }
        else
        {
            ESP_LOGI(TAG, "Kết nối Wi-Fi thất bại.");
        }
        // Xóa cờ nếu rớt mạng
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Kết nối thành công! Lấy được IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;

        // --- THÊM MỚI: Phất cờ báo hiệu đã có mạng ---
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Hàm xử lý sự kiện MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI("MQTT", "Đã kết nối Broker! Đang Subscribe...");
        esp_mqtt_client_subscribe(client, "test_topic/#", 0);
        break;
    case MQTT_EVENT_DATA:
        printf("Nhận được data: %.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI("MQTT", "Mất kết nối với Broker!");
        break;
    default:
        break;
    }
}

void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Khởi tạo Wi-Fi STA hoàn tất.");
}

void app_main(void)
{
    // Khởi tạo NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // --- THÊM MỚI: Khởi tạo Event Group trước khi gọi Wi-Fi ---
    s_wifi_event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "Bắt đầu khởi tạo Wi-Fi...");
    wifi_init_sta();

    ESP_LOGI(TAG, "Đang chờ kết nối Internet...");

    // --- THÊM MỚI: Chặn app_main lại cho đến khi có cờ WIFI_CONNECTED_BIT ---
    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,        // Không tự động xóa cờ sau khi đọc
                        pdFALSE,        // Chỉ cần 1 bit cờ là đủ
                        portMAX_DELAY); // Chờ vô thời hạn

    // Kể từ dòng này trở xuống, chắc chắn 100% ESP32 đã có mạng Internet
    ESP_LOGI(TAG, "Đã có mạng! Bắt đầu cấu hình MQTT...");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://mqtt.toolhub.app",
        // Thêm tài khoản và mật khẩu vào đây
        .credentials.username = "hungvm",
        .credentials.authentication.password = "20225198",

        //  Đặt ID cho thiết bị. Nếu không khai báo, ESP-IDF sẽ tự tạo ID dựa trên địa chỉ MAC.
        .credentials.client_id = "ESP32S3_01",
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    // Vòng lặp chính
    while (1)
    {
        // Gửi tin nhắn lên Broker mỗi 5 giây
        esp_mqtt_client_publish(client, "test_topic/", "Hello from IDF!", 0, 1, 0);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}