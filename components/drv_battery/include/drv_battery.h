#ifndef DRV_BATTERY_H
#define DRV_BATTERY_H

#include "esp_err.h"

/// Driver đọc điện áp pin qua ADC (oneshot) + hệ số cầu phân áp. Thuần phần cứng.
/// Chân ADC khai báo ở hardware_config.h (BATTERY_ADC_GPIO) — đổi pin chỉ cần sửa macro đó.

/**
 * @brief Khởi tạo ADC để đọc pin.
 * @param adc_gpio Chân GPIO nối tới nhánh giữa cầu phân áp (phải là chân ADC hợp lệ).
 * @param divider_ratio Hệ số phân áp (Vpin = Vadc × ratio). Cầu 100k:100k → 2.0.
 * @return ESP_OK nếu thành công; lỗi nếu GPIO không phải chân ADC.
 */
esp_err_t drv_battery_init(int adc_gpio, float divider_ratio);

/**
 * @brief Đọc điện áp pin (mV) đã nhân hệ số phân áp.
 * @return Điện áp pin tính bằng mV, hoặc -1 nếu chưa init / lỗi đọc.
 */
int drv_battery_read_mv(void);

/**
 * @brief Đọc mức pin ước lượng theo phần trăm (Li-ion 1 cell, ánh xạ tuyến tính tạm thời).
 * @return % pin (0–100), hoặc -1 nếu chưa init / lỗi đọc.
 */
int drv_battery_read_percent(void);

#endif  // DRV_BATTERY_H
