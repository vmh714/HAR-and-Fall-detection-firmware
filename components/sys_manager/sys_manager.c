#include "sys_manager.h"
#include "esp_log.h"
#include "esp_event.h"

extern uint32_t svc_cloud_get_stream_timeout(void);

static const char *TAG = "SYS_MANAGER";
static system_state_t current_state = STATE_INIT;

static system_state_t expected_state = STATE_NORMAL;

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
            ESP_LOGI(TAG, "Cloud MQTT connected. Transitioning to expected state: %d", expected_state);
            sys_manager_set_state(expected_state);
        } else if (event_id == CLOUD_CMD_START_STREAM) {
            ESP_LOGI(TAG, "Start streaming command received. Transitioning to STATE_STREAMING.");
            expected_state = STATE_STREAMING;
            sys_manager_set_state(STATE_STREAMING);
        } else if (event_id == CLOUD_CMD_STOP_STREAM) {
            ESP_LOGI(TAG, "Stop streaming command received. Transitioning to STATE_NORMAL.");
            expected_state = STATE_NORMAL;
            sys_manager_set_state(STATE_NORMAL);
        } else if (event_id == CLOUD_CMD_OTA_UPDATE) {
            ESP_LOGI(TAG, "OTA update command received. Transitioning to STATE_OTA.");
            expected_state = STATE_OTA;
            sys_manager_set_state(STATE_OTA);
        }
    // Nhóm event hệ thống: bắt lỗi phần cứng
    } else if (event_base == SYS_EVENT) {
        if (event_id == SYS_EVT_HARDWARE_ERROR) {
            ESP_LOGE(TAG, "Hardware error event received. Transitioning to STATE_ERROR.");
            sys_manager_set_state(STATE_ERROR);
        }
    }
}

#include "esp_timer.h"
#include <stdint.h>
#include "esp_system.h" // For esp_restart()

static esp_timer_handle_t stream_timeout_timer = NULL;
static esp_timer_handle_t auto_reboot_timer = NULL;

/// Thời điểm hết hạn cửa sổ comms-critical (micro-giây, monotonic).
/// 0 = không có cửa sổ nào đang hoạt động.
static volatile int64_t s_comms_critical_until_us = 0;

static void stream_timeout_callback(void* arg) {
    ESP_LOGW(TAG, "Stream timeout reached (%lu mins). Auto-stopping stream...", svc_cloud_get_stream_timeout());
    expected_state = STATE_NORMAL;
    sys_manager_set_state(STATE_NORMAL);
}

static void auto_reboot_callback(void* arg) {
    ESP_LOGE(TAG, "Auto-reboot timer expired. Restarting system...");
    esp_restart();
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
    esp_event_handler_register(SYS_EVENT, ESP_EVENT_ANY_ID, &sys_manager_event_handler, NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = &stream_timeout_callback,
        .name = "stream_timeout"
    };
    esp_timer_create(&timer_args, &stream_timeout_timer);

    const esp_timer_create_args_t auto_reboot_args = {
        .callback = &auto_reboot_callback,
        .name = "auto_reboot"
    };
    esp_timer_create(&auto_reboot_args, &auto_reboot_timer);

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
 * @brief Gia hạn cửa sổ comms-critical thêm ms milli-giây, dùng MAX để tránh hai writer giẫm nhau.
 * @param ms Độ dài cần giữ kể từ bây giờ (milli-giây).
 */
void sys_manager_bump_comms_critical(uint32_t ms) {
    int64_t new_deadline = esp_timer_get_time() + (int64_t)ms * 1000LL;
    if (new_deadline > s_comms_critical_until_us) {
        s_comms_critical_until_us = new_deadline;
    }
}

/**
 * @brief Kiểm tra xem thiết bị có đang trong cửa sổ comms-critical không.
 * @return true nếu đang trong cửa sổ, false nếu đã hết hoặc chưa bắt đầu.
 */
bool sys_manager_is_comms_critical(void) {
    return esp_timer_get_time() < s_comms_critical_until_us;
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
    
    if (current_state == STATE_STREAMING && new_state != STATE_STREAMING) {
        if (stream_timeout_timer) esp_timer_stop(stream_timeout_timer);
    }

    ESP_LOGI(TAG, "FSM State Transition: %d -> %d", current_state, new_state);
    current_state = new_state;
    
    if (new_state == STATE_STREAMING) {
        if (stream_timeout_timer) {
            esp_timer_stop(stream_timeout_timer);
            uint64_t to_us = (uint64_t)svc_cloud_get_stream_timeout() * 60ULL * 1000000ULL;
            esp_timer_start_once(stream_timeout_timer, to_us);
            ESP_LOGI(TAG, "Started stream timeout timer: %lu minutes", svc_cloud_get_stream_timeout());
        }
    } else if (new_state == STATE_ERROR) {
        if (auto_reboot_timer) {
            esp_timer_start_once(auto_reboot_timer, 10000000ULL); // 10s delay
            ESP_LOGE(TAG, "System entered STATE_ERROR. Auto-rebooting in 10 seconds...");
        }
    }
}
