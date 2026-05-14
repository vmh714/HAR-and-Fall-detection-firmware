#include "wifi_mqtt_service.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

// ======================== PRIVATE ========================

static const char *TAG = "WIFI_MQTT";

#define WIFI_CONNECTED_BIT  BIT0
#define MQTT_CONNECTED_BIT  BIT1
#define MAXIMUM_RETRY       5

static EventGroupHandle_t s_wifi_event_group;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;
static int s_retry_num = 0;
static const char *s_device_id = NULL;

// ---------- WiFi Event Handler ----------
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
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
            ESP_LOGI(TAG, "Retrying Wi-Fi connection... (%d/%d)", s_retry_num, MAXIMUM_RETRY);
        }
        else
        {
            ESP_LOGW(TAG, "Wi-Fi connection failed after %d retries.", MAXIMUM_RETRY);
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ---------- MQTT Event Handler ----------
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected to broker!");
        s_mqtt_connected = true;
        xEventGroupSetBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT Disconnected from broker!");
        s_mqtt_connected = false;
        xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT Data received on topic: %.*s", event->topic_len, event->topic);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error event!");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "  Transport error: 0x%x", event->error_handle->esp_transport_sock_errno);
            ESP_LOGE(TAG, "  TLS error: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(TAG, "  TLS stack error: 0x%x", event->error_handle->esp_tls_stack_err);
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "  Connection refused, code: 0x%x", event->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

// ---------- Khởi tạo WiFi STA ----------
static void wifi_init_sta(const char *ssid, const char *pass)
{
    // PRE-CONDITION: esp_netif_init() và esp_event_loop_create_default()
    // đã được gọi bởi app_main trước khi hàm này được gọi.
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    // Copy SSID và Password vào wifi_config (an toàn hơn gán trực tiếp)
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA initialized. Connecting to \"%s\"...", ssid);
}

// ======================== PUBLIC API ========================

esp_err_t wifi_mqtt_service_init(const wifi_mqtt_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;

    // Lưu device_id
    s_device_id = config->mqtt_client_id;

    // PRE-CONDITION: Caller (app_main) PHẢI đã gọi:
    //   - nvs_flash_init()
    //   - esp_netif_init()
    //   - esp_event_loop_create_default()
    // Để cho phép parallel boot (IMU init song song với WiFi init).

    // 1. Tạo Event Group để đồng bộ WiFi
    s_wifi_event_group = xEventGroupCreate();

    // 2. Khởi tạo WiFi STA
    ESP_LOGI(TAG, "Starting WiFi...");
    wifi_init_sta(config->wifi_ssid, config->wifi_pass);

    // 3. Chờ WiFi kết nối (BLOCK tại đây)
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdFALSE,
                        portMAX_DELAY);

    // 5. WiFi đã sẵn sàng → Khởi tạo MQTT
    ESP_LOGI(TAG, "WiFi connected! Starting MQTT client...");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = config->mqtt_broker_uri,
        .credentials.username = config->mqtt_username,
        .credentials.authentication.password = config->mqtt_password,
        .credentials.client_id = config->mqtt_client_id,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);

    // 6. Chờ MQTT kết nối tới broker (timeout 15 giây)
    ESP_LOGI(TAG, "Waiting for MQTT broker connection...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                        MQTT_CONNECTED_BIT,
                        pdFALSE,
                        pdFALSE,
                        pdMS_TO_TICKS(15000));

    if (bits & MQTT_CONNECTED_BIT) {
        ESP_LOGI(TAG, "MQTT ready! Device ID: \"%s\"", s_device_id);
    } else {
        ESP_LOGW(TAG, "MQTT connection timeout! Will retry in background.");
    }

    return ESP_OK;
}

bool wifi_mqtt_is_connected(void)
{
    return s_mqtt_connected;
}

int wifi_mqtt_publish(const char *topic, const char *data, int qos, int retain)
{
    if (!s_mqtt_connected || s_mqtt_client == NULL)
    {
        return -1;
    }
    return esp_mqtt_client_publish(s_mqtt_client, topic, data, 0, qos, retain);
}

const char *wifi_mqtt_get_device_id(void)
{
    return s_device_id;
}
