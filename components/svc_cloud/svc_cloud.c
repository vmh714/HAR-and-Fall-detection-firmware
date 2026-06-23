#include "svc_cloud.h"
#include "hardware_config.h"
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
#include "svc_network.h"
#include "drv_battery.h"
#include "svc_ota.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tflite_wrapper.h"

static const char* TAG = "SVC_CLOUD";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;
static TaskHandle_t s_cloud_task_handle = NULL;
static char s_device_id[64] = {0};
/// Hàng đợi IMU tách biệt: luồng IMU stream (STREAMING) đi qua queue riêng,
/// không dùng chung với telemetry/alert để tránh chặn lẫn nhau khi tải cao.
static QueueHandle_t s_imu_queue = NULL;
static uint32_t s_telemetry_interval_ms = 5000; // Mặc định 5s
static uint64_t s_last_fall_alert_ts = 0;
/// Cooldown chống spam alert: sau mỗi lần gửi cảnh báo ngã, im lặng cố định
/// một khoảng thời gian để một cú ngã không bị phát hiện lặp lại tạo ra hàng loạt alert trùng.
static uint64_t s_fall_cooldown_us = 15000000ULL;  // Mặc định 15 seconds

typedef struct
{
    char broker_uri[128];
    char client_id[64];
    char username[64];
    char password[64];
} mqtt_config_t;

static mqtt_config_t s_mqtt_cfg_data;

/**
 * @brief Xử lý các sự kiện từ MQTT client (kết nối, mất kết nối, nhận data, lỗi).
 *        Khi nhận lệnh trên topic command sẽ parse JSON và phát event điều khiển tương ứng.
 * @param handler_args Tham số người dùng truyền khi đăng ký (không dùng).
 * @param base Event base của MQTT.
 * @param event_id Mã sự kiện MQTT.
 * @param event_data Con trỏ tới dữ liệu sự kiện (esp_mqtt_event_handle_t).
 */
static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                               int32_t event_id, void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected to broker!");
            s_mqtt_connected = true;

            /// Subscribe topic command với QoS 1 để đảm bảo nhận đủ lệnh điều khiển
            /// từ backend (start/stop stream, set_interval, OTA).
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

            // Dựng lại topic command kỳ vọng để so khớp với topic nhận được
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
                                cJSON *url_item = cJSON_GetObjectItem(root, "url");
                                if (cJSON_IsString(url_item) && url_item->valuestring != NULL)
                                {
                                    ESP_LOGI(TAG, "Action: ota_update. URL: %s",
                                             url_item->valuestring);
                                    esp_event_post(CLOUD_EVENT, CLOUD_CMD_OTA_UPDATE,
                                                   NULL, 0, portMAX_DELAY);
                                    svc_ota_trigger(url_item->valuestring);
                                }
                                else
                                {
                                    ESP_LOGE(TAG, "ota_update: missing 'url' field");
                                }
                            }
                            else if (strcmp(action_item->valuestring, "set_interval") == 0)
                            {
                                cJSON* val_item = cJSON_GetObjectItem(root, "val");
                                if (cJSON_IsNumber(val_item))
                                {
                                    uint32_t new_interval_sec = val_item->valueint;
                                    // Chặn giá trị hợp lệ: từ 1s đến 3600s (1 giờ)
                                    if (new_interval_sec >= 1 && new_interval_sec <= 3600)
                                    {
                                        s_telemetry_interval_ms = new_interval_sec * 1000;
                                        ESP_LOGI(TAG, "Action: set_interval. New interval: %lu ms", s_telemetry_interval_ms);
                                        
                                        /// Lưu interval vào NVS để giữ cấu hình qua các lần
                                        /// khởi động lại thiết bị (đọc lại trong svc_cloud_init).
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
                            else if (strcmp(action_item->valuestring, "set_fall_threshold") == 0)
                            {
                                cJSON* val_item = cJSON_GetObjectItem(root, "val");
                                if (cJSON_IsNumber(val_item))
                                {
                                    float new_threshold = val_item->valuedouble;
                                    // Chặn giá trị hợp lệ: từ 0.15 đến 0.95
                                    if (new_threshold >= 0.15f && new_threshold <= 0.95f)
                                    {
                                        ESP_LOGI(TAG, "Action: set_fall_threshold. New threshold: %.2f", new_threshold);
                                        tflite_set_fall_threshold(new_threshold);
                                        
                                        /// Lưu vào NVS (lưu dạng integer phần trăm)
                                        nvs_handle_t my_handle;
                                        esp_err_t err = nvs_open("config", NVS_READWRITE, &my_handle);
                                        if (err == ESP_OK) {
                                            uint32_t thr_pct = (uint32_t)(new_threshold * 100.0f);
                                            nvs_set_u32(my_handle, "fall_thr", thr_pct);
                                            nvs_commit(my_handle);
                                            nvs_close(my_handle);
                                            ESP_LOGI(TAG, "Saved new fall threshold to NVS: %lu%%", thr_pct);
                                        } else {
                                            ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
                                        }
                                    }
                                    else
                                    {
                                        ESP_LOGW(TAG, "Invalid fall_threshold value: %.2f. Ignored.", new_threshold);
                                    }
                                }
                            }
                            else if (strcmp(action_item->valuestring, "set_fall_cooldown") == 0)
                            {
                                cJSON* val_item = cJSON_GetObjectItem(root, "val");
                                if (cJSON_IsNumber(val_item))
                                {
                                    uint32_t new_cooldown_sec = val_item->valueint;
                                    // Chặn giá trị hợp lệ: từ 5s đến 300s
                                    if (new_cooldown_sec >= 5 && new_cooldown_sec <= 300)
                                    {
                                        s_fall_cooldown_us = (uint64_t)new_cooldown_sec * 1000000ULL;
                                        ESP_LOGI(TAG, "Action: set_fall_cooldown. New cooldown: %lu s", new_cooldown_sec);
                                        
                                        /// Lưu vào NVS
                                        nvs_handle_t my_handle;
                                        esp_err_t err = nvs_open("config", NVS_READWRITE, &my_handle);
                                        if (err == ESP_OK) {
                                            nvs_set_u32(my_handle, "fall_cd", new_cooldown_sec);
                                            nvs_commit(my_handle);
                                            nvs_close(my_handle);
                                            ESP_LOGI(TAG, "Saved new fall cooldown to NVS: %lu s", new_cooldown_sec);
                                        } else {
                                            ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
                                        }
                                    }
                                    else
                                    {
                                        ESP_LOGW(TAG, "Invalid fall_cooldown value: %lu. Ignored.", new_cooldown_sec);
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

/**
 * @brief Task chính của dịch vụ Cloud: gửi IMU batch (chế độ STREAMING) và
 *        gửi telemetry định kỳ (chế độ NORMAL) lên MQTT broker.
 * @param pvParameters Tham số task FreeRTOS (không dùng).
 */
static void svc_cloud_task(void* pvParameters)
{
    imu_batch_data_t batch;
    ESP_LOGI(TAG, "Cloud Task started. Waiting for IMU batches...");

    TickType_t last_telemetry_time = xTaskGetTickCount();

    while (1)
    {
        /// Chờ batch IMU tối đa 1s rồi mới rơi xuống nhánh telemetry; timeout này
        /// đảm bảo telemetry vẫn được kiểm tra định kỳ ngay cả khi không có batch nào.
        if (xQueueReceive(s_imu_queue, &batch, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            /// Mất kết nối MQTT thì bỏ luôn batch (drop) thay vì tích lũy, tránh tràn
            /// bộ nhớ và gửi dữ liệu cũ không còn giá trị thời gian thực.
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

            /// Mã hóa Base64 dữ liệu IMU nhị phân để nhúng an toàn vào JSON
            /// (JSON không tải được byte thô). Lần gọi đầu chỉ để lấy độ dài b64_len.
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
                        /// IMU stream dùng QoS 0: dữ liệu tần suất cao, mất vài batch
                        /// không nghiêm trọng, ưu tiên thông lượng hơn độ tin cậy.
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
                    uint32_t walk_steps = 0, run_steps = 0;
                    imu_service_get_steps(&walk_steps, &run_steps);
                    int batt = drv_battery_read_percent();
                    cJSON_AddNumberToObject(root, "battery", batt < 0 ? 0 : batt);
                    /// walk_steps/run_steps tách riêng để backend tính distance đúng theo loại
                    /// (đi bộ 0.415 vs chạy 0.5 × chiều cao); "steps" = tổng để tương thích.
                    cJSON_AddNumberToObject(root, "walk_steps", walk_steps);
                    cJSON_AddNumberToObject(root, "run_steps", run_steps);
                    cJSON_AddNumberToObject(root, "steps", walk_steps + run_steps);
                    cJSON_AddStringToObject(root, "state", "NORMAL");
                    cJSON_AddStringToObject(root, "ai_pred", svc_ai_get_latest_prediction());
                    cJSON_AddNumberToObject(root, "ai_conf", svc_ai_get_latest_confidence());
                    cJSON_AddNumberToObject(root, "interval", s_telemetry_interval_ms / 1000);
                    cJSON_AddNumberToObject(root, "fall_threshold", tflite_get_fall_threshold());
                    cJSON_AddNumberToObject(root, "fall_cooldown", s_fall_cooldown_us / 1000000ULL);
                    cJSON_AddNumberToObject(root, "rssi", svc_network_get_rssi());
                    char* json_str = cJSON_PrintUnformatted(root);
                    if (json_str)
                    {
                        char topic[128];
                        snprintf(topic, sizeof(topic), "eldercare/%s/status",
                                 s_device_id);
                        /// Telemetry status dùng QoS 0: gửi định kỳ liên tục, mất một
                        /// gói sẽ được bù ở chu kỳ kế tiếp nên không cần đảm bảo gửi đến.
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

/**
 * @brief Xử lý sự kiện phát hiện ngã từ svc_ai: publish cảnh báo SOS lên MQTT
 *        với cơ chế cooldown chống spam.
 * @param arg Tham số người dùng khi đăng ký (không dùng).
 * @param event_base Event base nguồn (AI_EVENT).
 * @param event_id Mã sự kiện (AI_EVT_FALL_DETECTED).
 * @param event_data Dữ liệu sự kiện (không dùng).
 */
static void ai_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_base == AI_EVENT && event_id == AI_EVT_FALL_DETECTED)
    {
        if (!s_mqtt_connected)
            return;

        uint64_t now = esp_timer_get_time();
        /// Chỉ gửi nếu đã qua cooldown, hoặc đây là lần ngã đầu tiên (ts == 0).
        if (now - s_last_fall_alert_ts >= s_fall_cooldown_us ||
            s_last_fall_alert_ts == 0)
        {
            ESP_LOGW(TAG, "Fall event received! Publishing SOS alert...");
            cJSON* root = cJSON_CreateObject();
            if (root)
            {
                /// Payload khớp AlertPayload của backend (user_name/message bắt buộc
                /// dù backend chỉ dùng confidence). Bỏ timestamp: firmware chưa có RTC
                /// → backend tự dùng thời gian server.
                cJSON_AddStringToObject(root, "user_name", "");
                cJSON_AddStringToObject(root, "message", "Fall detected");
                cJSON_AddNumberToObject(root, "confidence", svc_ai_get_latest_confidence());
                char* json_str = cJSON_PrintUnformatted(root);
                if (json_str)
                {
                    char topic[128];
                    snprintf(topic, sizeof(topic), "eldercare/%s/alert/fall",
                             s_device_id);
                    /// Cảnh báo ngã dùng QoS 1: đây là dữ liệu sống còn, bắt buộc
                    /// đảm bảo đến được broker ít nhất một lần (khác với telemetry QoS 0).
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

/**
 * @brief Xử lý sự kiện mạng: khởi tạo & start MQTT client khi có mạng,
 *        dừng MQTT client khi mất mạng.
 * @param arg Tham số người dùng khi đăng ký (không dùng).
 * @param event_base Event base nguồn (NET_EVENT).
 * @param event_id Mã sự kiện mạng (kết nối WiFi/Cellular hoặc mất kết nối).
 * @param event_data Dữ liệu sự kiện (không dùng).
 */
static void net_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == NET_EVENT && (event_id == NET_EVT_WIFI_CONNECTED ||
                                    event_id == NET_EVT_CELLULAR_CONNECTED))
    {
        ESP_LOGI(TAG, "Network connected. Starting MQTT Client...");
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = s_mqtt_cfg_data.broker_uri,
            .broker.verification.certificate = CONFIG_MQTT_CA_CERT,
            .credentials.username = s_mqtt_cfg_data.username,
            .credentials.authentication.password = s_mqtt_cfg_data.password,
            .credentials.client_id = s_mqtt_cfg_data.client_id,
            .buffer.size = 2048,
            .buffer.out_size = 2048,
        };

        // Chỉ khởi tạo client một lần; lần mất/khôi phục mạng sau chỉ start lại
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

/**
 * @brief Đẩy một batch dữ liệu IMU vào hàng đợi để task cloud gửi đi (chế độ STREAMING).
 * @param batch_data Con trỏ tới batch dữ liệu IMU cần gửi.
 * @return ESP_OK nếu enqueue thành công; ESP_ERR_INVALID_STATE nếu hàng đợi chưa tạo;
 *         ESP_ERR_NO_MEM nếu hàng đợi đầy.
 */
esp_err_t svc_cloud_enqueue_imu_batch(const void* batch_data)
{
    if (s_imu_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    // Timeout 0: nếu queue đầy thì trả lỗi ngay, không chặn task gọi
    if (xQueueSend(s_imu_queue, batch_data, 0) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief Khởi tạo dịch vụ Cloud: lưu cấu hình MQTT, nạp interval telemetry từ NVS,
 *        tạo hàng đợi IMU, đăng ký các event handler mạng/AI và tạo task xử lý.
 * @param broker_uri URI của MQTT broker.
 * @param client_id Định danh client MQTT, đồng thời dùng làm device_id trong topic.
 * @param username Tên đăng nhập MQTT.
 * @param password Mật khẩu MQTT.
 * @return ESP_OK nếu khởi tạo thành công; ESP_ERR_NO_MEM nếu tạo hàng đợi thất bại.
 */
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

    /// Nạp lại interval telemetry đã lưu trong NVS (nếu có) để giữ cấu hình người
    /// dùng từng đặt qua lệnh set_interval; nếu chưa có thì dùng mặc định 5s.
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("config", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        nvs_get_u32(my_handle, "tel_int", &s_telemetry_interval_ms);
        
        uint32_t fall_thr_pct = 60; // Mặc định 0.6
        if (nvs_get_u32(my_handle, "fall_thr", &fall_thr_pct) == ESP_OK) {
            tflite_set_fall_threshold((float)fall_thr_pct / 100.0f);
            ESP_LOGI(TAG, "Loaded fall threshold from NVS: %.2f", (float)fall_thr_pct / 100.0f);
        }
        
        uint32_t fall_cooldown_sec = 15; // Mặc định 15s
        if (nvs_get_u32(my_handle, "fall_cd", &fall_cooldown_sec) == ESP_OK) {
            s_fall_cooldown_us = (uint64_t)fall_cooldown_sec * 1000000ULL;
            ESP_LOGI(TAG, "Loaded fall cooldown from NVS: %lu s", fall_cooldown_sec);
        }
        
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Loaded telemetry interval from NVS: %lu ms", s_telemetry_interval_ms);
    } else {
        ESP_LOGI(TAG, "No saved telemetry interval, using default: %lu ms", s_telemetry_interval_ms);
    }

    /// Hàng đợi IMU sâu 5 phần tử: đủ đệm vài batch khi mạng tắc nghẽn tạm thời,
    /// vượt quá sẽ drop ở enqueue thay vì làm nghẽn task sản xuất dữ liệu.
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

/**
 * @brief Kiểm tra trạng thái kết nối tới MQTT broker.
 * @return true nếu đang kết nối, false nếu mất kết nối.
 */
bool svc_cloud_is_connected(void)
{
    return s_mqtt_connected;
}

/**
 * @brief Publish một message lên MQTT broker.
 * @param topic Topic MQTT đích.
 * @param data Nội dung payload.
 * @param qos Mức QoS mong muốn cho topic này.
 * @param retain Cờ retain (1 = giữ message trên broker).
 * @return msg_id của message, hoặc -1 nếu chưa kết nối / client chưa sẵn sàng.
 */
int svc_cloud_publish(const char* topic, const char* data, int qos, int retain)
{
    if (!s_mqtt_connected || s_mqtt_client == NULL)
    {
        return -1;
    }
    return esp_mqtt_client_publish(s_mqtt_client, topic, data, 0, qos, retain);
}
