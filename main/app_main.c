#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"

// Component headers
#include "sys_manager.h"

static const char *TAG = "MAIN_APP";

// ======================== PHASE 1 TEST CODE ========================
// Task giả lập các service phát sự kiện (Mạng kết nối, MQTT kết nối...)
static void test_fsm_task(void *pvParameters) {
    ESP_LOGW("TEST_FSM", "Waiting 3 seconds before simulating network connect...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    ESP_LOGW("TEST_FSM", "Simulating Network Connected Event...");
    esp_event_post(NET_EVENT, NET_EVT_WIFI_CONNECTED, NULL, 0, portMAX_DELAY);
    
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGW("TEST_FSM", "Simulating Cloud MQTT Connected Event...");
    esp_event_post(CLOUD_EVENT, CLOUD_EVT_MQTT_CONNECTED, NULL, 0, portMAX_DELAY);

    vTaskDelete(NULL);
}

// Event Handler nhận sự kiện và đổi State tương ứng
static void fsm_test_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == NET_EVENT && event_id == NET_EVT_WIFI_CONNECTED) {
        ESP_LOGI("EVENT_HANDLER", "Received NET_EVT_WIFI_CONNECTED. Transitioning to STATE_CONNECTING...");
        sys_manager_set_state(STATE_CONNECTING);
    } 
    else if (event_base == CLOUD_EVENT && event_id == CLOUD_EVT_MQTT_CONNECTED) {
        ESP_LOGI("EVENT_HANDLER", "Received CLOUD_EVT_MQTT_CONNECTED. Transitioning to STATE_NORMAL...");
        sys_manager_set_state(STATE_NORMAL);
    }
}
// ====================================================================


// ======================== APP MAIN ========================
void app_main(void)
{
    ESP_LOGI(TAG, "==== ESP32-S3 ELDERCARE FIRMWARE ====");

    // 1. Khởi tạo các hệ thống nền tảng của ESP-IDF (NVS, Netif)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());

    // 2. Khởi tạo FSM Trung tâm (Event Loop)
    sys_manager_init();
    
    // Đăng ký Event Handler để test (Thực tế logic này sẽ nằm trong svc_network / svc_cloud)
    esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID, &fsm_test_event_handler, NULL);
    esp_event_handler_register(CLOUD_EVENT, ESP_EVENT_ANY_ID, &fsm_test_event_handler, NULL);

    // 3. Khởi tạo các Service (Chưa implement, tạm thời để comment)
    // svc_network_init();
    // svc_cloud_init();
    // svc_imu_init();
    // svc_ai_init();

    ESP_LOGI(TAG, "System Skeleton Initialized. Main thread yields.");

    // Chạy Task test giả lập sự kiện
    xTaskCreate(test_fsm_task, "test_fsm_task", 2048, NULL, 5, NULL);
}
