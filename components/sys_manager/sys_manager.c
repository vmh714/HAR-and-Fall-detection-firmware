#include "sys_manager.h"
#include "esp_log.h"
#include "esp_event.h"

static const char *TAG = "SYS_MANAGER";
static system_state_t current_state = STATE_INIT;

/// Định nghĩa (cấp phát) các Event Base đã khai báo trong header.
/// Kiến trúc event-driven: các service giao tiếp lỏng (loose coupling) qua
/// Default Event Loop thay vì gọi hàm trực tiếp, giúp tách biệt module hoàn toàn.
ESP_EVENT_DEFINE_BASE(SYS_EVENT);
ESP_EVENT_DEFINE_BASE(NET_EVENT);
ESP_EVENT_DEFINE_BASE(CLOUD_EVENT);
ESP_EVENT_DEFINE_BASE(IMU_EVENT);
ESP_EVENT_DEFINE_BASE(AI_EVENT);

/**
 * @brief Handler trung tâm của FSM: nhận event NET/CLOUD và tự động chuyển trạng thái.
 * @param arg        Tham số người dùng truyền khi đăng ký (không dùng).
 * @param event_base Nhóm event phát sinh (NET_EVENT hoặc CLOUD_EVENT).
 * @param event_id   Mã định danh event trong nhóm.
 * @param event_data Dữ liệu kèm theo event (không dùng ở đây).
 */
static void sys_manager_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    /// Handler tự động lái FSM theo event: sự kiện NET/CLOUD quyết định trạng thái kế tiếp,
    /// nhờ đó logic chuyển trạng thái tập trung tại một nơi duy nhất.
    // Nhóm event mạng: có/mất kết nối đều đưa về STATE_CONNECTING để chờ Cloud
    if (event_base == NET_EVENT) {
        if (event_id == NET_EVT_WIFI_CONNECTED || event_id == NET_EVT_CELLULAR_CONNECTED) {
            ESP_LOGI(TAG, "Network connected. Transitioning to STATE_CONNECTING (awaiting Cloud)...");
            sys_manager_set_state(STATE_CONNECTING);
        } else if (event_id == NET_EVT_DISCONNECTED) {
            ESP_LOGW(TAG, "Network disconnected. Transitioning to STATE_CONNECTING...");
            sys_manager_set_state(STATE_CONNECTING); // Retry kết nối lại
        }
    // Nhóm event cloud: trạng thái MQTT và lệnh điều khiển luồng từ backend
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
        } else if (event_id == CLOUD_CMD_OTA_UPDATE) {
            ESP_LOGI(TAG, "OTA update command received. Transitioning to STATE_OTA.");
            sys_manager_set_state(STATE_OTA);
        }
    }
}

/**
 * @brief Khởi tạo Default Event Loop và máy trạng thái (FSM) trung tâm.
 *
 * Tạo event loop mặc định và đăng ký handler trung tâm cho các nhóm event
 * NET_EVENT và CLOUD_EVENT, sau đó đặt trạng thái ban đầu là STATE_INIT.
 */
void sys_manager_init(void) {
    ESP_LOGI(TAG, "Initializing System Event Loop & FSM...");

    /// Default Event Loop là trung tâm điều phối: mọi service post event vào đây,
    /// nhờ vậy các module không phụ thuộc trực tiếp lẫn nhau (loose coupling).
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);  // Chỉ chấp nhận ESP_OK hoặc loop đã tồn tại sẵn
    }

    // Đăng ký handler trung tâm của FSM cho toàn bộ event mạng và cloud
    esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID, &sys_manager_event_handler, NULL);
    esp_event_handler_register(CLOUD_EVENT, ESP_EVENT_ANY_ID, &sys_manager_event_handler, NULL);

    current_state = STATE_INIT;
    ESP_LOGI(TAG, "FSM initialized. Current state: STATE_INIT");
}

/**
 * @brief Lấy trạng thái hiện tại của FSM.
 * @return Trạng thái hệ thống hiện tại (system_state_t).
 */
system_state_t sys_manager_get_state(void) {
    return current_state;
}

/**
 * @brief Đặt trạng thái mới cho FSM.
 * @param new_state Trạng thái đích cần chuyển tới.
 */
void sys_manager_set_state(system_state_t new_state) {
    // Tránh set lại state cũ gây nhiễu log
    if (current_state == new_state) {
        return;
    }

    ESP_LOGI(TAG, "FSM State Transition: %d -> %d", current_state, new_state);
    current_state = new_state;
}
