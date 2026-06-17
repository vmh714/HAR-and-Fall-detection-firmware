# Component: drv_battery

## Tổng Quan
Driver đọc mức pin qua **ADC oneshot** + hệ số cầu phân áp. Thuần phần cứng, không business logic. Chân ADC khai báo ở `main/hardware_config.h` (`BATTERY_ADC_GPIO`) — **đổi pin chỉ cần sửa macro đó** (driver tự suy ra ADC unit/channel từ GPIO qua `adc_oneshot_io_to_channel`).

## Public API
- **`esp_err_t drv_battery_init(int adc_gpio, float divider_ratio)`** — cấu hình ADC (atten 12dB, 12-bit) + hiệu chuẩn curve-fitting. `divider_ratio` = hệ số cầu phân áp (2× 100kΩ → `2.0`).
- **`int drv_battery_read_mv(void)`** — điện áp pin (mV) đã nhân hệ số phân áp; `-1` nếu lỗi.
- **`int drv_battery_read_percent(void)`** — % pin Li-ion 1 cell (ánh xạ tuyến tính 3.3V→0%, 4.2V→100%); `-1` nếu lỗi.

## Tích hợp
- `app_main.c`: `drv_battery_init(BATTERY_ADC_GPIO, BATTERY_DIVIDER_RATIO)`.
- `svc_cloud.c`: telemetry gọi `drv_battery_read_percent()` cho field `battery`.

> ⚠️ Ánh xạ %-điện áp hiện là tuyến tính tạm thời (Li-ion phi tuyến) — tinh chỉnh bằng bảng tra sau khi đo thực tế. Chân `BATTERY_ADC_GPIO` đang để placeholder `GPIO_NUM_1`, đổi sang chân hàn thật.

> **Cập nhật cuối (Timestamp):** 2026-06-17
