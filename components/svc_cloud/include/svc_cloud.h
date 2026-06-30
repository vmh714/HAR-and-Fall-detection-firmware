#ifndef SVC_CLOUD_H
#define SVC_CLOUD_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Khởi tạo dịch vụ Cloud: lưu cấu hình MQTT, nạp interval telemetry từ NVS,
 *        tạo hàng đợi IMU, đăng ký các event handler và tạo task xử lý.
 * @param broker_uri URI của MQTT broker (vd: "mqtt://host:1883").
 * @param client_id Định danh client MQTT, đồng thời dùng làm device_id trong topic.
 * @param username Tên đăng nhập MQTT.
 * @param password Mật khẩu MQTT.
 * @return ESP_OK nếu khởi tạo thành công; mã lỗi tương ứng nếu thất bại.
 */
esp_err_t svc_cloud_init(const char *broker_uri, const char *client_id, const char *username, const char *password);

/**
 * @brief Truyền một chuỗi log bất kỳ lên topic eldercare/{id}/command để debug từ xa.
 * @param msg Chuỗi log cần gửi.
 */
void svc_cloud_send_log(const char *msg);

/**
 * @brief Nếu hệ thống chuẩn bị sập (restart), hàm này sẽ kiểm tra xem có cảnh báo
 * ngã nào còn vướng trong RAM chưa gửi được không, nếu có thì ghi chặt vào NVS flash.
 */
void svc_cloud_flush_cache_to_nvs(void);

/**
 * @brief Kiểm tra trạng thái kết nối tới MQTT broker.
 * @return true nếu đang kết nối, false nếu mất kết nối.
 */
bool svc_cloud_is_connected(void);

/**
 * @brief Publish một message lên MQTT broker.
 * @param topic Topic MQTT đích.
 * @param data Nội dung payload.
 * @param qos Mức QoS mong muốn cho topic này.
 * @param retain Cờ retain (1 = giữ message trên broker).
 * @return msg_id của message, hoặc -1 nếu chưa kết nối / client chưa sẵn sàng.
 */
int svc_cloud_publish(const char *topic, const char *data, int qos, int retain);

/**
 * @brief Đẩy một batch dữ liệu IMU vào hàng đợi để task cloud gửi đi (chế độ STREAMING).
 * @param batch_data Con trỏ tới batch dữ liệu IMU cần gửi.
 * @return ESP_OK nếu enqueue thành công; ESP_ERR_INVALID_STATE nếu hàng đợi chưa tạo;
 *         ESP_ERR_NO_MEM nếu hàng đợi đầy.
 */
esp_err_t svc_cloud_enqueue_imu_batch(const void *batch_data);

/**
 * @brief Lấy thời gian timeout cho luồng stream (phút) cấu hình từ NVS.
 * @return Giá trị timeout (phút).
 */
uint32_t svc_cloud_get_stream_timeout(void);

#endif // SVC_CLOUD_H
