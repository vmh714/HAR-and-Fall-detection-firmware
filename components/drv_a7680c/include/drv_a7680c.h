#ifndef DRV_A7680C_H
#define DRV_A7680C_H

#include "esp_err.h"
#include "driver/gpio.h"

/// Driver điều khiển module 4G LTE A7680C qua chân RST (GPIO thuần).
/// Lớp này KHÔNG đụng tới UART/AT — phần giao tiếp dữ liệu do tầng trên (esp_modem
/// trong svc_network) đảm nhiệm. Việc tách lớp giúp driver chỉ lo reset phần cứng.

/**
 * @brief Khởi tạo chân RST điều khiển module A7680C.
 * @param rst_pin Chân GPIO nối tới RST của module.
 *
 * Cấu hình chân ở chế độ output, tắt pull/ngắt, đặt mức nghỉ (cao). Phải gọi
 * trước drv_a7680c_reset().
 */
void drv_a7680c_init(gpio_num_t rst_pin);

/**
 * @brief Reset module bằng phần cứng: kéo chân RST xuống LOW trong 250ms rồi nhả.
 * @param did_reset Con trỏ để nhận kết quả xem có thực sự reset phần cứng hay không.
 * @return ESP_OK nếu đã phát xung; ESP_ERR_INVALID_STATE nếu chưa init.
 *
 * @note Sau khi gọi cần chờ ~5–10s cho module boot xong (UART/AT sẵn sàng)
 *       trước khi tạo esp_modem DCE.
 */
esp_err_t drv_a7680c_reset(bool *did_reset);

/**
 * @brief Gửi khẩn cấp một gói tin MQTT qua tập lệnh AT nội bộ của module (AT+CMQTT...).
 *        Chỉ dùng khi PPPoS đã rớt và ESP32 đang chuẩn bị reset.
 * @param dce_ptr Con trỏ esp_modem_dce_t từ svc_network.
 * @param broker Chuỗi TCP của broker (VD: "tcp://broker.hivemq.com:1883").
 * @param topic Chuỗi MQTT topic.
 * @param payload Chuỗi JSON/nội dung gói tin.
 */
esp_err_t drv_a7680c_emergency_mqtt_publish(void *dce_ptr, const char *broker, const char *topic, const char *payload);

#endif  // DRV_A7680C_H
