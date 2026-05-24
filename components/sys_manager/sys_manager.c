#include "sys_manager.h"
#include "esp_log.h"
#include "esp_event.h"

static const char *TAG = "SYS_MANAGER";
static system_state_t current_state = STATE_INIT;

// Define the Event Bases
ESP_EVENT_DEFINE_BASE(SYS_EVENT);
ESP_EVENT_DEFINE_BASE(NET_EVENT);
ESP_EVENT_DEFINE_BASE(CLOUD_EVENT);
ESP_EVENT_DEFINE_BASE(IMU_EVENT);
ESP_EVENT_DEFINE_BASE(AI_EVENT);

static void sys_manager_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == NET_EVENT) {
        if (event_id == NET_EVT_WIFI_CONNECTED || event_id == NET_EVT_CELLULAR_CONNECTED) {
            ESP_LOGI(TAG, "Network connected. Transitioning to STATE_CONNECTING (awaiting Cloud)...");
            sys_manager_set_state(STATE_CONNECTING);
        } else if (event_id == NET_EVT_DISCONNECTED) {
            ESP_LOGW(TAG, "Network disconnected. Transitioning to STATE_CONNECTING...");
            sys_manager_set_state(STATE_CONNECTING); // Retrying connection
        }
    } else if (event_base == CLOUD_EVENT) {
        if (event_id == CLOUD_EVT_MQTT_CONNECTED) {
            ESP_LOGI(TAG, "Cloud MQTT connected. Transitioning to STATE_NORMAL.");
            sys_manager_set_state(STATE_NORMAL);
        } else if (event_id == CLOUD_CMD_START_STREAM) {
            ESP_LOGI(TAG, "Start streaming command received. Transitioning to STATE_STREAMING.");
            sys_manager_set_state(STATE_STREAMING);
        } else if (event_id == CLOUD_CMD_STOP_STREAM) {
            ESP_LOGI(TAG, "Stop streaming command received. Transitioning to STATE_NORMAL.");
            sys_manager_set_state(STATE_NORMAL);
        }
    }
}

void sys_manager_init(void) {
    ESP_LOGI(TAG, "Initializing System Event Loop & FSM...");
    
    // Khởi tạo Event Loop mặc định (Trung tâm điều phối Event)
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    
    // Đăng ký Central Event Handler cho FSM
    esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID, &sys_manager_event_handler, NULL);
    esp_event_handler_register(CLOUD_EVENT, ESP_EVENT_ANY_ID, &sys_manager_event_handler, NULL);
    
    current_state = STATE_INIT;
    ESP_LOGI(TAG, "FSM initialized. Current state: STATE_INIT");
}

system_state_t sys_manager_get_state(void) {
    return current_state;
}

void sys_manager_set_state(system_state_t new_state) {
    // Tránh set lại state cũ gây nhiễu log
    if (current_state == new_state) {
        return;
    }
    
    ESP_LOGI(TAG, "FSM State Transition: %d -> %d", current_state, new_state);
    current_state = new_state;
}
