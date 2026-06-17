# Component: drv_a7680c

## Tổng Quan
Tầng điều khiển **nguồn** (power control) cho module 4G LTE A7680C. Driver chỉ lo bật/tắt module qua chân **PWRKEY** (GPIO thuần), **không** đụng tới UART/AT command — phần giao tiếp dữ liệu và PPPoS do tầng trên (`esp_modem` trong `svc_network`) đảm nhiệm. Việc tách lớp giúp driver chỉ chịu trách nhiệm phần cứng, không chứa business logic.

Driver phụ thuộc `driver/gpio.h` (xem `include/drv_a7680c.h`).

## Public API
- **`void drv_a7680c_init(gpio_num_t pwrkey_pin)`**: cấu hình chân PWRKEY thành output, tắt pull/ngắt, đặt mức nghỉ (cao). Lưu chân vào biến static. Pin thực tế truyền từ `hardware_config.h` (`A7680C_PWRKEY_PIN = GPIO_NUM_38`).
- **`esp_err_t drv_a7680c_power_on(void)`**: phát xung PWRKEY mức thấp **~100ms** (Ton typ. 50ms theo datasheet A7680C) rồi nhả về cao. Sau khi gọi cần chờ ~5–10s cho module boot xong UART. Trả `ESP_ERR_INVALID_STATE` nếu chưa init.
- **`esp_err_t drv_a7680c_power_off(void)`**: phát xung PWRKEY mức thấp **≥2.5s** (Toff min) để tắt module. Phải chờ **≥2s** (Toff-on) trước khi bật lại.

> ⚠️ **Cực tính phụ thuộc board.** Mã giả định PWRKEY active-low (mức cao = nghỉ, xung thấp = kích). Nếu board đảo mức thì hoán đổi 0/1 trong `drv_a7680c.c`. Timing đã theo datasheet A7680C (Ton ~50ms, Toff ≥2.5s, Toff-on ≥2s) — xem `atcommand.md`.

## Quan hệ với PPPoS (svc_network)
`drv_a7680c` chỉ bật nguồn module. Toàn bộ AT command (gồm `AT+CNMP=38` khóa LTE-only), cấu hình PDP/APN, dial PPP và netif do `svc_network_init_cellular()` thực hiện qua `esp_modem`. Xem `protocol.md` và `system_integration.md`.

> **Cập nhật cuối (Timestamp):** 2026-06-17
