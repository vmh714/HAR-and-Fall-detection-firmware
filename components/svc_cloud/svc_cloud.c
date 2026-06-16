#include "svc_cloud.h"
#include <string.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "imu_service.h"
#include "mbedtls/base64.h"
#include "mqtt_client.h"
#include "sys_manager.h"
#include "svc_ai.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "SVC_CLOUD";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;
static TaskHandle_t s_cloud_task_handle = NULL;
static char s_device_id[64] = {0};
static QueueHandle_t s_imu_queue = NULL;
static uint32_t s_telemetry_interval_ms = 5000; // Mặc định 5s

typedef struct
{
    char broker_uri[128];
    char client_id[64];
    char username[64];
    char password[64];
} mqtt_config_t;

static mqtt_config_t s_mqtt_cfg_data;

static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                               int32_t event_id, void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected to broker!");
            s_mqtt_connected = true;

            // Subscribe to cmd topic
            char cmd_topic[128];
            snprintf(cmd_topic, sizeof(cmd_topic), "eldercare/%s/command",
                     s_device_id);
            esp_mqtt_client_subscribe(s_mqtt_client, cmd_topic, 1);
            ESP_LOGI(TAG, "Subscribed to topic: %s", cmd_topic);

            esp_event_post(CLOUD_EVENT, CLOUD_EVT_MQTT_CONNECTED, NULL, 0,
                           portMAX_DELAY);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected from broker!");
            s_mqtt_connected = false;
            break;

        case MQTT_EVENT_DATA:
        {
            ESP_LOGI(TAG, "MQTT Data received on topic: %.*s", event->topic_len,
                     event->topic);

            char cmd_topic_check[128];
            snprintf(cmd_topic_check, sizeof(cmd_topic_check),
                     "eldercare/%s/command", s_device_id);

            if (strncmp(event->topic, cmd_topic_check, event->topic_len) == 0)
            {
                char* json_str = malloc(event->data_len + 1);
                if (json_str != NULL)
                {
                    memcpy(json_str, event->data, event->data_len);
                    json_str[event->data_len] = '\0';

                    cJSON* root = cJSON_Parse(json_str);
                    if (root != NULL)
                    {
                        cJSON* action_item =
                            cJSON_GetObjectItem(root, "action");
                        if (cJSON_IsString(action_item) &&
                            (action_item->valuestring != NULL))
                        {
                            if (strcmp(action_item->valuestring,
                                       "start_stream") == 0)
                            {
                                ESP_LOGI(TAG,
                                         "Action: start_stream. Posting "
                                         "CLOUD_CMD_START_STREAM event...");
                                esp_event_post(CLOUD_EVENT,
                                               CLOUD_CMD_START_STREAM, NULL, 0,
                                               portMAX_DELAY);
                            }
                            else if (strcmp(action_item->valuestring,
                                            "stop_stream") == 0)
                            {
                                ESP_LOGI(TAG,
                                         "Action: stop_stream. Posting "
                                         "CLOUD_CMD_STOP_STREAM event...");
                                esp_event_post(CLOUD_EVENT,
                                               CLOUD_CMD_STOP_STREAM, NULL, 0,
                                               portMAX_DELAY);
                            }
                            else if (strcmp(action_item->valuestring,
                                            "ota_update") == 0)
                            {
                                ESP_LOGI(TAG,
                                         "Action: ota_update (Not implemented "
                                         "yet - Phase 5.1)");
                                // TODO: Thêm logic cập nhật OTA ở Phase 5.1 (vd
                                // parse URL từ cJSON)
                            }
                            else if (strcmp(action_item->valuestring, "set_interval") == 0)
                            {
                                cJSON* val_item = cJSON_GetObjectItem(root, "val");
                                if (cJSON_IsNumber(val_item))
                                {
                                    uint32_t new_interval_sec = val_item->valueint;
                                    if (new_interval_sec >= 1 && new_interval_sec <= 3600)
                                    {
                                        s_telemetry_interval_ms = new_interval_sec * 1000;
                                        ESP_LOGI(TAG, "Action: set_interval. New interval: %lu ms", s_telemetry_interval_ms);
                                        
                                        // Ghi đè vào NVS
                                        nvs_handle_t my_handle;
                                        esp_err_t err = nvs_open("config", NVS_READWRITE, &my_handle);
                                        if (err == ESP_OK) {
                                            nvs_set_u32(my_handle, "tel_int", s_telemetry_interval_ms);
                                            nvs_commit(my_handle);
                                            nvs_close(my_handle);
                                            ESP_LOGI(TAG, "Saved new telemetry interval to NVS");
                                        } else {
                                            ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
                                        }
                                    }
                                }
                            }
                            else
                            {
                                ESP_LOGW(TAG, "Unknown action: %s",
                                         action_item->valuestring);
                            }
                        }
                        cJSON_Delete(root);
                    }
                    else
                    {
                        ESP_LOGE(TAG, "JSON parse error!");
                    }
                    free(json_str);
                }
            }
            break;
        }
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error event!");
            break;
        default:
            break;
    }
}

static void svc_cloud_task(void* pvParameters)
{
    imu_batch_data_t batch;
    ESP_LOGI(TAG, "Cloud Task started. Waiting for IMU batches...");

    TickType_t last_telemetry_time = xTaskGetTickCount();

    while (1)
    {
        if (xQueueReceive(s_imu_queue, &batch, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            if (!s_mqtt_connected || s_mqtt_client == NULL)
            {
                ESP_LOGW(TAG,
                         "MQTT disconnected. Dropping IMU batch (count: %d)",
                         batch.count);
                continue;
            }

            if (batch.count == 0)
                continue;

            size_t raw_len = batch.count * sizeof(imu_stream_data_t);
            size_t b64_len = 0;

            mbedtls_base64_encode(NULL, 0, &b64_len,
                                  (const unsigned char*)batch.data, raw_len);

            char* b64_str = malloc(b64_len + 1);
            if (b64_str)
            {
                size_t olen = 0;
                mbedtls_base64_encode((unsigned char*)b64_str, b64_len, &olen,
                                      (const unsigned char*)batch.data,
                                      raw_len);
                b64_str[olen] = '\0';

                cJSON* root = cJSON_CreateObject();
                if (root)
                {
                    cJSON_AddNumberToObject(root, "ts",
                                            esp_timer_get_time() / 1000);
                    cJSON_AddNumberToObject(root, "fs", 100);
                    cJSON_AddNumberToObject(root, "cnt", batch.count);
                    cJSON_AddStringToObject(root, "data_b64", b64_str);

                    char* json_str = cJSON_PrintUnformatted(root);
                    if (json_str)
                    {
                        char topic[128];
                        snprintf(topic, sizeof(topic),
                                 "eldercare/%s/imu_stream", s_device_id);
                        int msg_id = esp_mqtt_client_publish(
                            s_mqtt_client, topic, json_str, 0, 0, 0);
                        ESP_LOGI(
                            TAG,
                            "Published IMU batch (len: %zu) to %s, msg_id=%d",
                            strlen(json_str), topic, msg_id);
                        free(json_str);
                    }
                    cJSON_Delete(root);
                }
                free(b64_str);
            }
        }

        // --- Task 4.4: Telemetry (Pedometer & Battery) ---
        // Gửi status theo chu kỳ s_telemetry_interval_ms khi đang ở STATE_NORMAL
        if (s_mqtt_connected &&
            (xTaskGetTickCount() - last_telemetry_time >= pdMS_TO_TICKS(s_telemetry_interval_ms)))
        {
            if (sys_manager_get_state() == STATE_NORMAL)
            {
                cJSON* root = cJSON_CreateObject();
                if (root)
                {
                    cJSON_AddNumberToObject(root, "battery",
                                            100);               // Placeholder
                    cJSON_AddNumberToObject(root, "steps", 0);  // Placeholder
                    cJSON_AddStringToObject(root, "state", "NORMAL");
                    cJSON_AddStringToObject(root, "ai_pred", svc_ai_get_latest_prediction());
                    cJSON_AddNumberToObject(root, "ai_conf", svc_ai_get_latest_confidence());
                    cJSON_AddNumberToObject(root, "interval", s_telemetry_interval_ms / 1000);
                    char* json_str = cJSON_PrintUnformatted(root);
                    if (json_str)
                    {
                        char topic[128];
                        snprintf(topic, sizeof(topic), "eldercare/%s/status",
                                 s_device_id);
                        esp_mqtt_client_publish(s_mqtt_client, topic, json_str,
                                                0, 0, 0);
                        ESP_LOGI(TAG, "Published Telemetry (Status): %s",
                                 json_str);
                        free(json_str);
                    }
                    cJSON_Delete(root);
                }
            }
            last_telemetry_time = xTaskGetTickCount();
        }
    }
}

static uint64_t s_last_fall_alert_ts = 0;
#define FALL_COOLDOWN_US (15000000ULL)  // 15 seconds

static void ai_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_base == AI_EVENT && event_id == AI_EVT_FALL_DETECTED)
    {
        if (!s_mqtt_connected)
            return;

        uint64_t now = esp_timer_get_time();
        if (now - s_last_fall_alert_ts >= FALL_COOLDOWN_US ||
            s_last_fall_alert_ts == 0)
        {
            ESP_LOGW(TAG, "Fall event received! Publishing SOS alert...");
            cJSON* root = cJSON_CreateObject();
            if (root)
            {
                cJSON_AddStringToObject(root, "alert", "FALL");
                cJSON_AddNumberToObject(root, "ts", now / 1000);
                char* json_str = cJSON_PrintUnformatted(root);
                if (json_str)
                {
                    char topic[128];
                    snprintf(topic, sizeof(topic), "eldercare/%s/alert",
                             s_device_id);
                    esp_mqtt_client_publish(s_mqtt_client, topic, json_str, 0,
                                            1, 0);  // QoS 1
                    free(json_str);
                }
                cJSON_Delete(root);
            }
            s_last_fall_alert_ts = now;
        }
        else
        {
            ESP_LOGI(TAG, "Fall event ignored (Cooldown active)");
        }
    }
}

static void net_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == NET_EVENT && (event_id == NET_EVT_WIFI_CONNECTED ||
                                    event_id == NET_EVT_CELLULAR_CONNECTED))
    {
        ESP_LOGI(TAG, "Network connected. Starting MQTT Client...");
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = s_mqtt_cfg_data.broker_uri,
            .credentials.username = s_mqtt_cfg_data.username,
            .credentials.authentication.password = s_mqtt_cfg_data.password,
            .credentials.client_id = s_mqtt_cfg_data.client_id,
            .buffer.size = 2048,
            .buffer.out_size = 2048,
        };

        if (s_mqtt_client == NULL)
        {
            s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
            if (s_mqtt_client != NULL)
            {
                esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                               mqtt_event_handler, NULL);
            }
        }
        if (s_mqtt_client != NULL)
        {
            esp_mqtt_client_start(s_mqtt_client);
        }
    }
    else if (event_base == NET_EVENT && event_id == NET_EVT_DISCONNECTED)
    {
        if (s_mqtt_client != NULL && s_mqtt_connected)
        {
            ESP_LOGI(TAG, "Network disconnected. Stopping MQTT Client...");
            esp_mqtt_client_stop(s_mqtt_client);
            s_mqtt_connected = false;
        }
    }
}

esp_err_t svc_cloud_enqueue_imu_batch(const void* batch_data)
{
    if (s_imu_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_imu_queue, batch_data, 0) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t svc_cloud_init(const char* broker_uri, const char* client_id,
                         const char* username, const char* password)
{
    ESP_LOGI(TAG, "Initializing Cloud Service...");

    strncpy(s_mqtt_cfg_data.broker_uri, broker_uri,
            sizeof(s_mqtt_cfg_data.broker_uri) - 1);
    strncpy(s_mqtt_cfg_data.client_id, client_id,
            sizeof(s_mqtt_cfg_data.client_id) - 1);
    strncpy(s_mqtt_cfg_data.username, username,
            sizeof(s_mqtt_cfg_data.username) - 1);
    strncpy(s_mqtt_cfg_data.password, password,
            sizeof(s_mqtt_cfg_data.password) - 1);
    strncpy(s_device_id, client_id, sizeof(s_device_id) - 1);

    // Read saved interval from NVS
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("config", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        nvs_get_u32(my_handle, "tel_int", &s_telemetry_interval_ms);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Loaded telemetry interval from NVS: %lu ms", s_telemetry_interval_ms);
    } else {
        ESP_LOGI(TAG, "No saved telemetry interval, using default: %lu ms", s_telemetry_interval_ms);
    }

    s_imu_queue = xQueueCreate(5, sizeof(imu_batch_data_t));
    if (s_imu_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create IMU queue");
        return ESP_ERR_NO_MEM;
    }

    esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID, &net_event_handler,
                               NULL);
    esp_event_handler_register(AI_EVENT, AI_EVT_FALL_DETECTED,
                               &ai_event_handler, NULL);

    xTaskCreate(svc_cloud_task, "svc_cloud_task", 4096, NULL, 5,
                &s_cloud_task_handle);

    return ESP_OK;
}

bool svc_cloud_is_connected(void)
{
    return s_mqtt_connected;
}

int svc_cloud_publish(const char* topic, const char* data, int qos, int retain)
{
    if (!s_mqtt_connected || s_mqtt_client == NULL)
    {
        return -1;
    }
    return esp_mqtt_client_publish(s_mqtt_client, topic, data, 0, qos, retain);
}
