#ifndef DRV_A7680C_H
#define DRV_A7680C_H

#include "esp_err.h"
#include "driver/gpio.h"

/// Driver điều khiển NGUỒN module 4G LTE A7680C qua chân PWRKEY (GPIO thuần).
/// Lớp này KHÔNG đụng tới UART/AT — phần giao tiếp dữ liệu do tầng trên (esp_modem
/// trong svc_network) đảm nhiệm. Việc tách lớp giúp driver chỉ lo bật/tắt phần cứng.

/**
 * @brief Khởi tạo chân PWRKEY điều khiển nguồn A7680C.
 * @param pwrkey_pin Chân GPIO nối tới PWRKEY của module.
 *
 * Cấu hình chân ở chế độ output, tắt pull/ngắt, đặt mức nghỉ (cao). Phải gọi
 * trước drv_a7680c_power_on()/power_off().
 */
void drv_a7680c_init(gpio_num_t pwrkey_pin);

/**
 * @brief Bật nguồn module: phát xung PWRKEY mức thấp ~100ms (Ton typ. 50ms) rồi nhả.
 * @return ESP_OK nếu đã phát xung; ESP_ERR_INVALID_STATE nếu chưa init.
 *
 * @note Sau khi gọi cần chờ ~5–10s cho module boot xong (UART/AT sẵn sàng)
 *       trước khi tạo esp_modem DCE.
 */
esp_err_t drv_a7680c_power_on(void);

/**
 * @brief Tắt nguồn module: phát xung PWRKEY mức thấp ~2.5s.
 * @return ESP_OK nếu đã phát xung; ESP_ERR_INVALID_STATE nếu chưa init.
 */
esp_err_t drv_a7680c_power_off(void);

#endif  // DRV_A7680C_H
