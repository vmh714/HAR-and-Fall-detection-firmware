#ifndef SYS_MANAGER_H
#define SYS_MANAGER_H

#include "esp_event.h"

/// Tập trạng thái của máy trạng thái hữu hạn (FSM) điều phối toàn hệ thống.
/// Mỗi trạng thái xác định hành vi vận hành hiện tại của thiết bị.
typedef enum {
    STATE_INIT,        ///< Khởi tạo phần cứng, driver và các service ban đầu
    STATE_CONNECTING,  ///< Chờ mạng + MQTT sẵn sàng, tự động retry kết nối
    STATE_NORMAL,      ///< Vận hành chính: AI inference liên tục, gửi telemetry, cảnh báo ngã
    STATE_STREAMING,   ///< Chế độ thu thập dữ liệu: batch raw IMU lên MQTT để train model
    STATE_OTA,         ///< Nhận firmware mới qua OTA rồi restart
    STATE_ERROR        ///< Trạng thái lỗi
} system_state_t;

/// Khai báo các Event Base — mỗi nhóm chức năng (hệ thống, mạng, cloud, IMU, AI)
/// có một base riêng để định tuyến event đúng handler qua Default Event Loop.
ESP_EVENT_DECLARE_BASE(SYS_EVENT);
ESP_EVENT_DECLARE_BASE(NET_EVENT);
ESP_EVENT_DECLARE_BASE(CLOUD_EVENT);
ESP_EVENT_DECLARE_BASE(IMU_EVENT);
ESP_EVENT_DECLARE_BASE(AI_EVENT);

/// Các event thuộc nhóm hệ thống (SYS_EVENT).
typedef enum {
    SYS_EVT_READY,             ///< Toàn bộ service đã sẵn sàng
    SYS_EVT_ENTER_STREAM_MODE, ///< Yêu cầu chuyển sang chế độ streaming
    SYS_EVT_ENTER_NORMAL_MODE  ///< Yêu cầu quay về chế độ vận hành bình thường
} sys_event_id_t;

/// Các event thuộc nhóm mạng (NET_EVENT) — kích hoạt chuyển trạng thái kết nối.
typedef enum {
    NET_EVT_WIFI_CONNECTED,     ///< WiFi đã kết nối (môi trường dev)
    NET_EVT_CELLULAR_CONNECTED, ///< 4G/LTE đã kết nối (môi trường production)
    NET_EVT_DISCONNECTED        ///< Mất kết nối mạng
} net_event_id_t;

/// Các event thuộc nhóm cloud (CLOUD_EVENT) — gồm trạng thái MQTT và lệnh điều khiển từ backend.
typedef enum {
    CLOUD_EVT_MQTT_CONNECTED, ///< Phiên MQTT tới broker đã thiết lập
    CLOUD_CMD_START_STREAM,   ///< Lệnh từ backend: bắt đầu streaming raw IMU
    CLOUD_CMD_STOP_STREAM,    ///< Lệnh từ backend: dừng streaming, về chế độ bình thường
    CLOUD_CMD_OTA_UPDATE      ///< Lệnh từ backend: flash firmware mới qua OTA
} cloud_event_id_t;

/// Các event thuộc nhóm IMU (IMU_EVENT) — báo dữ liệu cảm biến đã sẵn sàng.
typedef enum {
    IMU_EVT_BATCH_READY,  ///< Một batch raw IMU đã sẵn sàng để stream
    IMU_EVT_WINDOW_READY  ///< Một cửa sổ trượt (sliding window) đã sẵn sàng cho inference
} imu_event_id_t;

/// Các event thuộc nhóm AI (AI_EVENT) — kết quả suy luận của mô hình.
typedef enum {
    AI_EVT_FALL_DETECTED ///< Mô hình phát hiện sự kiện té ngã
} ai_event_id_t;

/**
 * @brief Khởi tạo Default Event Loop và máy trạng thái (FSM) trung tâm.
 *
 * Tạo event loop mặc định và đăng ký handler trung tâm cho các nhóm event
 * NET_EVENT và CLOUD_EVENT, sau đó đặt trạng thái ban đầu là STATE_INIT.
 */
void sys_manager_init(void);

/**
 * @brief Lấy trạng thái hiện tại của FSM.
 * @return Trạng thái hệ thống hiện tại (system_state_t).
 */
system_state_t sys_manager_get_state(void);

/**
 * @brief Đặt trạng thái mới cho FSM.
 * @param new_state Trạng thái đích cần chuyển tới.
 */
void sys_manager_set_state(system_state_t new_state);

#endif // SYS_MANAGER_H
