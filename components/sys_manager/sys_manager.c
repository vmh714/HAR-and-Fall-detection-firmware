#include "sys_manager.h"
#include "esp_log.h"

static const char *TAG = "SYS_MANAGER";
static system_state_t current_state = STATE_INIT;

// Define the Event Bases
ESP_EVENT_DEFINE_BASE(SYS_EVENT);
ESP_EVENT_DEFINE_BASE(NET_EVENT);
ESP_EVENT_DEFINE_BASE(CLOUD_EVENT);
ESP_EVENT_DEFINE_BASE(IMU_EVENT);
ESP_EVENT_DEFINE_BASE(AI_EVENT);

void sys_manager_init(void) {
    ESP_LOGI(TAG, "Initializing System Event Loop & FSM...");
    
    // Khởi tạo Event Loop mặc định (Trung tâm điều phối Event)
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    
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
