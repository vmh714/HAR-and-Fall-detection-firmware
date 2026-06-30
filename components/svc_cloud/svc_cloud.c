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
#include "esp_app_desc.h"
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
static uint32_t s_stream_timeout_min = 5;

static char s_cached_alert_payload[256] = {0};
static volatile bool s_has_cached_alert = false;
static int s_alert_msg_id = -1;

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

            char cfg_topic[128];
            snprintf(cfg_topic, sizeof(cfg_topic), "eldercare/%s/config/set",
                     s_device_id);
            esp_mqtt_client_subscribe(s_mqtt_client, cfg_topic, 1);
            ESP_LOGI(TAG, "Subscribed to topic: %s", cfg_topic);

            // Gửi config status ngay khi kết nối
            char cfg_status_topic[128];
            snprintf(cfg_status_topic, sizeof(cfg_status_topic), "eldercare/%s/config/status", s_device_id);
            cJSON* root_cfg = cJSON_CreateObject();
            if (root_cfg) {
                cJSON_AddNumberToObject(root_cfg, "interval", s_telemetry_interval_ms / 1000);
                cJSON_AddNumberToObject(root_cfg, "fall_threshold", tflite_get_fall_threshold());
                cJSON_AddNumberToObject(root_cfg, "fall_cooldown", s_fall_cooldown_us / 1000000ULL);
                cJSON_AddNumberToObject(root_cfg, "stream_timeout", s_stream_timeout_min);
                cJSON_AddNumberToObject(root_cfg, "fall_confirm_window", svc_ai_get_confirm_window_ms() / 1000);
                cJSON_AddNumberToObject(root_cfg, "rssi_interval", svc_network_get_rssi_interval_ms() / 1000);
                /// Báo version firmware đang chạy kèm config/status (lúc connect/reconnect) →
                /// backend tự cập nhật Device.firmware_version (đúng sau mỗi OTA reboot).
                cJSON_AddStringToObject(root_cfg, "fw_version", esp_app_get_description()->version);
                char* json_str = cJSON_PrintUnformatted(root_cfg);
                if (json_str) {
                    esp_mqtt_client_publish(s_mqtt_client, cfg_status_topic, json_str, 0, 1, 0);
                    free(json_str);
                }
                cJSON_Delete(root_cfg);
            }

            esp_event_post(CLOUD_EVENT, CLOUD_EVT_MQTT_CONNECTED, NULL, 0,
                           portMAX_DELAY);
            
            // 1. Kiểm tra xem NVS có cục Alert bị kẹt từ lần sập nguồn trước không
            nvs_handle_t my_handle;
            if (nvs_open("config", NVS_READWRITE, &my_handle) == ESP_OK) {
                size_t required_size = 0;
                if (nvs_get_str(my_handle, "unsent_al", NULL, &required_size) == ESP_OK && required_size > 0) {
                    char* old_alert = malloc(required_size);
                    if (nvs_get_str(my_handle, "unsent_al", old_alert, &required_size) == ESP_OK) {
                        s_alert_msg_id = esp_mqtt_client_publish(s_mqtt_client, cmd_topic, old_alert, 0, 1, 0); // tạm dùng biến tạm, sẽ ghi đè topic ngay
                        char al_topic[128];
                        snprintf(al_topic, sizeof(al_topic), "eldercare/%s/alert/fall", s_device_id);
                        s_alert_msg_id = esp_mqtt_client_publish(s_mqtt_client, al_topic, old_alert, 0, 1, 0);
                        ESP_LOGW(TAG, "PHỤC HỒI ALERT TỪ NVS: Gửi lại cảnh báo ngã cũ (msg_id=%d)", s_alert_msg_id);
                        s_has_cached_alert = true;
                        strncpy(s_cached_alert_payload, old_alert, sizeof(s_cached_alert_payload) - 1);
                        nvs_erase_key(my_handle, "unsent_al");
                        nvs_commit(my_handle);
                    }
                    free(old_alert);
                }
                nvs_close(my_handle);
            }
            
            // 2. Nếu trong phiên này có Alert chưa gửi được vì rớt mạng MQTT ngắn hạn
            if (s_has_cached_alert) {
                char al_topic[128];
                snprintf(al_topic, sizeof(al_topic), "eldercare/%s/alert/fall", s_device_id);
                s_alert_msg_id = esp_mqtt_client_publish(s_mqtt_client, al_topic, s_cached_alert_payload, 0, 1, 0);
                ESP_LOGW(TAG, "Đã gửi lại Alert từ RAM Cache (msg_id=%d)", s_alert_msg_id);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected from broker!");
            s_mqtt_connected = false;
            break;

        case MQTT_EVENT_DATA:
        {
            ESP_LOGI(TAG, "MQTT Data received on topic: %.*s", event->topic_len,
                     event->topic);

            // Dựng lại topic command và config kỳ vọng để so khớp với topic nhận được
            char cmd_topic_check[128];
            snprintf(cmd_topic_check, sizeof(cmd_topic_check),
                     "eldercare/%s/command", s_device_id);
                     
            char cfg_topic_check[128];
            snprintf(cfg_topic_check, sizeof(cfg_topic_check),
                     "eldercare/%s/config/set", s_device_id);

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
                                            "update_firmware") == 0)
                            {
                                cJSON* url_item = cJSON_GetObjectItem(root, "url");
                                if (cJSON_IsString(url_item) && (url_item->valuestring != NULL))
                                {
                                    ESP_LOGI(TAG, "Action: update_firmware. URL: %s", url_item->valuestring);
                                    
                                    // Gọi thư viện OTA để spawn task download & flash
                                    #include "svc_ota.h"
                                    esp_err_t err = svc_ota_trigger(url_item->valuestring);
                                    if (err != ESP_OK) {
                                        ESP_LOGE(TAG, "Failed to trigger OTA: %s", esp_err_to_name(err));
                                    } else {
                                        ESP_LOGI(TAG, "OTA triggered successfully. System will reboot upon completion.");
                                        // Gửi trạng thái phản hồi nếu cần thiết
                                    }
                                }
                                else
                                {
                                    ESP_LOGW(TAG, "Action update_firmware requires a valid 'url' string field.");
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
            else if (strncmp(event->topic, cfg_topic_check, event->topic_len) == 0)
            {
                char* json_str = malloc(event->data_len + 1);
                if (json_str != NULL)
                {
                    memcpy(json_str, event->data, event->data_len);
                    json_str[event->data_len] = '\0';

                    cJSON* root = cJSON_Parse(json_str);
                    if (root != NULL)
                    {
                        bool cfg_changed = false;
                        cJSON* item;
                        
                        item = cJSON_GetObjectItem(root, "interval");
                        if (cJSON_IsNumber(item)) {
                            uint32_t val = item->valueint;
                            if (val >= 1 && val <= 3600) {
                                s_telemetry_interval_ms = val * 1000;
                                cfg_changed = true;
                            }
                        }
                        
                        item = cJSON_GetObjectItem(root, "fall_threshold");
                        if (cJSON_IsNumber(item)) {
                            float val = item->valuedouble;
                            if (val >= 0.15f && val <= 0.95f) {
                                tflite_set_fall_threshold(val);
                                cfg_changed = true;
                            }
                        }
                        
                        item = cJSON_GetObjectItem(root, "fall_cooldown");
                        if (cJSON_IsNumber(item)) {
                            uint32_t val = item->valueint;
                            if (val >= 5 && val <= 300) {
                                s_fall_cooldown_us = (uint64_t)val * 1000000ULL;
                                cfg_changed = true;
                            }
                        }
                        
                        item = cJSON_GetObjectItem(root, "stream_timeout");
                        if (cJSON_IsNumber(item)) {
                            uint32_t val = item->valueint;
                            if (val >= 1 && val <= 60) {
                                s_stream_timeout_min = val;
                                cfg_changed = true;
                            }
                        }

                        item = cJSON_GetObjectItem(root, "fall_confirm_window");
                        if (cJSON_IsNumber(item)) {
                            uint32_t val = item->valueint;
                            if (val >= 1 && val <= 15) {
                                svc_ai_set_confirm_window_ms(val * 1000);
                                cfg_changed = true;
                            }
                        }

                        item = cJSON_GetObjectItem(root, "rssi_interval");
                        if (cJSON_IsNumber(item)) {
                            uint32_t val = (uint32_t)item->valueint;
                            /// 0 = tắt hẳn; giá trị hợp lệ khác: 60, 120, 300, 600 (giây).
                            /// svc_network_set_rssi_interval_ms tự clamp non-zero < 60s lên 60s.
                            if (val == 0 || (val >= 60 && val <= 600)) {
                                svc_network_set_rssi_interval_ms(val * 1000);
                                cfg_changed = true;
                            }
                        }

                        if (cfg_changed) {
                            nvs_handle_t my_handle;
                            if (nvs_open("config", NVS_READWRITE, &my_handle) == ESP_OK) {
                                nvs_set_u32(my_handle, "tel_int", s_telemetry_interval_ms);
                                nvs_set_u32(my_handle, "fall_thr", (uint32_t)(tflite_get_fall_threshold() * 100.0f));
                                nvs_set_u32(my_handle, "fall_cd", (uint32_t)(s_fall_cooldown_us / 1000000ULL));
                                nvs_set_u32(my_handle, "str_to", s_stream_timeout_min);
                                nvs_set_u32(my_handle, "fall_cf", svc_ai_get_confirm_window_ms());
                                nvs_set_u32(my_handle, "rssi_int", svc_network_get_rssi_interval_ms() / 1000);
                                nvs_commit(my_handle);
                                nvs_close(my_handle);
                                ESP_LOGI(TAG, "Saved new config to NVS");
                            }
                            
                            // Phản hồi lại status
                            char cfg_status_topic[128];
                            snprintf(cfg_status_topic, sizeof(cfg_status_topic), "eldercare/%s/config/status", s_device_id);
                            cJSON* root_cfg = cJSON_CreateObject();
                            if (root_cfg) {
                                cJSON_AddNumberToObject(root_cfg, "interval", s_telemetry_interval_ms / 1000);
                                cJSON_AddNumberToObject(root_cfg, "fall_threshold", tflite_get_fall_threshold());
                                cJSON_AddNumberToObject(root_cfg, "fall_cooldown", s_fall_cooldown_us / 1000000ULL);
                                cJSON_AddNumberToObject(root_cfg, "stream_timeout", s_stream_timeout_min);
                                cJSON_AddNumberToObject(root_cfg, "fall_confirm_window", svc_ai_get_confirm_window_ms() / 1000);
                                cJSON_AddNumberToObject(root_cfg, "rssi_interval", svc_network_get_rssi_interval_ms() / 1000);
                                cJSON_AddStringToObject(root_cfg, "fw_version", esp_app_get_description()->version);
                                char* reply_str = cJSON_PrintUnformatted(root_cfg);
                                if (reply_str) {
                                    esp_mqtt_client_publish(s_mqtt_client, cfg_status_topic, reply_str, 0, 1, 0);
                                    free(reply_str);
                                }
                                cJSON_Delete(root_cfg);
                            }
                        }
                        cJSON_Delete(root);
                    }
                    else
                    {
                        ESP_LOGE(TAG, "JSON parse error for config/set!");
                    }
                    free(json_str);
                }
            }
            break;
        }
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            if (event->msg_id == s_alert_msg_id) {
                ESP_LOGI(TAG, "Cảnh báo ngã đã được Broker xác nhận (QoS 1). Xóa cache.");
                s_has_cached_alert = false;
            }
            break;
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

    uint32_t last_sent_walk = 0, last_sent_run = 0;
    // Lấy giá trị ban đầu làm mốc để tính delta
    imu_service_get_steps(&last_sent_walk, &last_sent_run);
    
    int64_t mqtt_down_since_us = 0;
    static uint32_t s_stream_seq = 0;
    static system_state_t last_state = STATE_INIT;

    while (1)
    {
        system_state_t current_state = sys_manager_get_state();
        if (current_state == STATE_STREAMING && last_state != STATE_STREAMING) {
            s_stream_seq = 0;
        }
        last_state = current_state;
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
                    cJSON_AddNumberToObject(root, "seq", s_stream_seq++);
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
                        uint64_t t1 = esp_timer_get_time();
                        int msg_id = esp_mqtt_client_publish(
                            s_mqtt_client, topic, json_str, 0, 0, 0);
                        uint64_t t2 = esp_timer_get_time();

                        if (t2 - t1 > 500000ULL) {
                            ESP_LOGE(TAG, "Publish mất %llu ms! Mạng 4G quá tải không đáp ứng nổi luồng Stream liên tục. Tự động ngắt Stream để bảo toàn tính nhân quả của dữ liệu...", (t2 - t1)/1000);
                            esp_event_post(SYS_EVENT, CLOUD_CMD_STOP_STREAM, NULL, 0, portMAX_DELAY);
                        }

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
                    uint32_t current_walk = 0, current_run = 0;
                    imu_service_get_steps(&current_walk, &current_run);
                    
                    // Tính số bước đi được trong chu kỳ (interval) này
                    uint32_t delta_walk = current_walk - last_sent_walk;
                    uint32_t delta_run  = current_run  - last_sent_run;
                    
                    // Cập nhật lại mốc
                    last_sent_walk = current_walk;
                    last_sent_run  = current_run;

                    int batt = drv_battery_read_percent();
                    cJSON_AddNumberToObject(root, "battery", batt < 0 ? 0 : batt);
                    /// Gửi lên broker là số delta để backend cộng dồn vào InfluxDB
                    /// tránh bị cộng dồn trùng lặp số tổng.
                    cJSON_AddNumberToObject(root, "walk_steps", delta_walk);
                    cJSON_AddNumberToObject(root, "run_steps", delta_run);
                    cJSON_AddNumberToObject(root, "steps", delta_walk + delta_run);
                    cJSON_AddStringToObject(root, "state", "NORMAL");
                    cJSON_AddStringToObject(root, "ai_pred", svc_ai_get_latest_prediction());
                    cJSON_AddNumberToObject(root, "ai_conf", svc_ai_get_latest_confidence());
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

        // Watchdog: mạng nói còn kết nối nhưng MQTT chết kéo dài → tự phục hồi (tránh kẹt phải reset tay)
        if (!s_mqtt_connected && svc_network_is_connected()) {
            if (mqtt_down_since_us == 0) mqtt_down_since_us = esp_timer_get_time();
            int64_t down_ms = (esp_timer_get_time() - mqtt_down_since_us) / 1000;

            // Bậc 1 (~60s): ép socket/TLS mới sạch
            if (down_ms > 60000 && down_ms <= 150000) {
                static int64_t last_kick_us = 0;
                if (esp_timer_get_time() - last_kick_us > 30000000LL) { // tối đa 30s/lần
                    ESP_LOGW(TAG, "Watchdog: MQTT kẹt %lld ms → stop/start client", down_ms);
                    esp_mqtt_client_stop(s_mqtt_client);
                    esp_mqtt_client_start(s_mqtt_client);
                    last_kick_us = esp_timer_get_time();
                }
            }
            // Bậc 2 (~150s): A7680C/PPP wedge ở tầng dưới → chỉ reboot mới dọn
            else if (down_ms > 150000) {
                ESP_LOGE(TAG, "Watchdog: MQTT kẹt %lld ms → flush cache + SYS_EVT_HARDWARE_ERROR", down_ms);
                svc_cloud_flush_cache_to_nvs();
                esp_event_post(SYS_EVENT, SYS_EVT_HARDWARE_ERROR, NULL, 0, portMAX_DELAY);
            }
        } else {
            mqtt_down_since_us = 0; // reset khi MQTT up (ca mất mạng thật đã có đường NET_EVT_DISCONNECTED xử lý)
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
        uint64_t now = esp_timer_get_time();
        /// Chỉ gửi nếu đã qua cooldown, hoặc đây là lần ngã đầu tiên (ts == 0).
        if (now - s_last_fall_alert_ts >= s_fall_cooldown_us ||
            s_last_fall_alert_ts == 0)
        {
            ESP_LOGW(TAG, "Fall event received! Processing SOS alert...");
            cJSON* root = cJSON_CreateObject();
            if (root)
            {
                cJSON_AddStringToObject(root, "user_name", "");
                cJSON_AddStringToObject(root, "message", "Fall detected");
                cJSON_AddNumberToObject(root, "confidence", svc_ai_get_latest_confidence());
                char* json_str = cJSON_PrintUnformatted(root);
                if (json_str)
                {
                    // LƯU CACHE RAM NGAY LẬP TỨC ĐỂ TRÁNH MẤT NẾU ĐANG CHỜ MẠNG
                    strncpy(s_cached_alert_payload, json_str, sizeof(s_cached_alert_payload) - 1);
                    s_has_cached_alert = true;
                    s_alert_msg_id = -1;
                    
                    if (s_mqtt_connected) {
                        char topic[128];
                        snprintf(topic, sizeof(topic), "eldercare/%s/alert/fall", s_device_id);
                        s_alert_msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, json_str, 0, 1, 0);
                        ESP_LOGI(TAG, "Đã gửi MQTT Alert (msg_id=%d)", s_alert_msg_id);
                        /// Giữ link MQTT suốt cooldown để alert QoS1 chắc chắn đi và tránh trễ alert lặp.
                        sys_manager_bump_comms_critical((uint32_t)(s_fall_cooldown_us / 1000ULL));
                    } else {
                        ESP_LOGW(TAG, "Mất mạng, Alert tạm lưu vào RAM Cache: %s", json_str);
                    }
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
            .session.keepalive            = 45,     // mặc định 120 → phát hiện socket chết nhanh hơn
            .network.timeout_ms           = 15000,  // dư địa handshake TLS trên link 4G chậm
            .network.reconnect_timeout_ms = 8000,
            .task.priority                = 6,
            .task.stack_size              = 6144,
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

        /// (Tùy chọn — multi-org) Override broker/creds từ NVS nếu được pre-provision.
        /// Không có key → giữ default #define CONFIG_MQTT_BROKER_URI (= broker org).
        size_t len = sizeof(s_mqtt_cfg_data.broker_uri);
        if (nvs_get_str(my_handle, "mqtt_uri", s_mqtt_cfg_data.broker_uri, &len) == ESP_OK)
            ESP_LOGI(TAG, "Loaded broker URI from NVS: %s", s_mqtt_cfg_data.broker_uri);
        len = sizeof(s_mqtt_cfg_data.username);
        nvs_get_str(my_handle, "mqtt_user", s_mqtt_cfg_data.username, &len);
        len = sizeof(s_mqtt_cfg_data.password);
        nvs_get_str(my_handle, "mqtt_pass", s_mqtt_cfg_data.password, &len);

        uint32_t fall_thr_pct = 25; // Mặc định 0.25
        if (nvs_get_u32(my_handle, "fall_thr", &fall_thr_pct) == ESP_OK) {
            tflite_set_fall_threshold((float)fall_thr_pct / 100.0f);
            ESP_LOGI(TAG, "Loaded fall threshold from NVS: %.2f", (float)fall_thr_pct / 100.0f);
        }
        
        uint32_t fall_cooldown_sec = 15; // Mặc định 15s
        if (nvs_get_u32(my_handle, "fall_cd", &fall_cooldown_sec) == ESP_OK) {
            s_fall_cooldown_us = (uint64_t)fall_cooldown_sec * 1000000ULL;
            ESP_LOGI(TAG, "Loaded fall cooldown from NVS: %lu s", fall_cooldown_sec);
        }
        
        uint32_t stream_to = 5;
        if (nvs_get_u32(my_handle, "str_to", &stream_to) == ESP_OK) {
            s_stream_timeout_min = stream_to;
            ESP_LOGI(TAG, "Loaded stream timeout from NVS: %lu min", stream_to);
        }

        uint32_t fall_cf_ms = 4000;
        if (nvs_get_u32(my_handle, "fall_cf", &fall_cf_ms) == ESP_OK) {
            svc_ai_set_confirm_window_ms(fall_cf_ms);
            ESP_LOGI(TAG, "Loaded fall confirm window from NVS: %lu ms", fall_cf_ms);
        }

        uint32_t rssi_int_sec = 300;
        if (nvs_get_u32(my_handle, "rssi_int", &rssi_int_sec) == ESP_OK) {
            svc_network_set_rssi_interval_ms(rssi_int_sec * 1000);
            ESP_LOGI(TAG, "Loaded RSSI interval from NVS: %lu s", rssi_int_sec);
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

void svc_cloud_flush_cache_to_nvs(void)
{
    if (s_has_cached_alert) {
        nvs_handle_t my_handle;
        if (nvs_open("config", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_str(my_handle, "unsent_al", s_cached_alert_payload);
            nvs_commit(my_handle);
            nvs_close(my_handle);
            ESP_LOGE(TAG, "CRITICAL: Đã khoá chặt Alert vào NVS trước khi Restart mạch!");
        }
    }
}

uint32_t svc_cloud_get_stream_timeout(void)
{
    return s_stream_timeout_min;
}
