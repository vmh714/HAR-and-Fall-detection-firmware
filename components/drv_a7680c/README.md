# Component: drv_a7680c

> **Cập nhật cuối (Timestamp):** 2026-06-30
> **Trạng thái:** Hoạt động ổn định (No-op RST)

---

## 1. PUBLIC API

- **`void drv_a7680c_init(gpio_num_t rst_pin)`**: Cấu hình chân RST nối tới module thành output. Việc thiết lập mức cao (idle) bị bỏ qua để tránh sự cố sập nguồn của module.
- **`esp_err_t drv_a7680c_reset(bool *did_reset)`**: No-op (chỉ ghi log cảnh báo và gán `*did_reset = false`). Quyết định không can thiệp vào chân RST được chốt để đảm bảo hoạt động ổn định của module 4G.
- **`esp_err_t drv_a7680c_emergency_mqtt_publish(void *dce_ptr, const char *broker, const char *topic, const char *payload)`**: Sử dụng `esp_modem_at` để gửi bản tin MQTT trực tiếp qua tập lệnh AT nội bộ (`AT+CMQTT...`) của A7680C. Thường dùng khi PPP/HDLC interface bị hỏng nhưng tầng AT command vẫn phản hồi, giúp thiết bị gửi cảnh báo cuối cùng trước khi tự reset.

---

## 2. Mục đích (Purpose / Why)
Tầng điều khiển phần cứng cho module 4G LTE A7680C. 
Thay vì bao gồm toàn bộ stack mạng (vốn đã được `svc_network` lo liệu thông qua `esp_modem`), module này chỉ tập trung vào việc quản lý phần cứng vật lý (các chân RST, PWRKEY) và cung cấp các tập lệnh AT khẩn cấp (Emergency AT Commands) khi PPPoS bị sập.

## 3. Cơ chế cốt lõi (How it works)
* **Khởi động tự động (Auto-Power-On):** Do mạch thực tế không nối dây PWRKEY vào MCU, module 4G được cấu hình bằng phần cứng để tự bật lên ngay khi có điện. MCU không cần (và không thể) điều khiển việc bật tắt này.
* **No-op Reset (Vô hiệu hóa reset cứng):** Trong thực tế, chạm vào chân RST sẽ khiến module A7680C tắt nguồn hẳn mà không tự khởi động lại được (xem quyết định thiết kế `D-019`). Do đó, module cố tình thiết kế hàm `drv_a7680c_reset()` thành **No-op** (không làm gì cả, chỉ ghi log) để bảo vệ hệ thống khỏi việc sập mạng vĩnh viễn.
* **Gửi MQTT Khẩn cấp (Emergency Publish):** Khi kết nối PPP (internet ảo) bị lỗi nhưng cổng Serial UART nối với A7680C vẫn sống, driver cung cấp luồng đi "cửa sau". Nó bỏ qua PPP và bắn trực tiếp các lệnh AT (`AT+CMQTT...`) xuống thẳng module 4G để nhờ module 4G tự kết nối MQTT và gửi cảnh báo cuối cùng (last-gasp) trước khi ESP32 tự khởi động lại.

## 4. Đa nhiệm & Tài nguyên (Concurrency & Resources)
* Hoàn toàn đồng bộ (Synchronous), không tạo bất kỳ Task nào. 
* Lệnh khẩn cấp `emergency_mqtt_publish` khóa UART mutex nội bộ của `esp_modem` để đảm bảo không đụng độ với các luồng đang cố đọc AT Command khác.

## 5. Luồng giao tiếp (Data Flow)
* **Input / Output:** Giao tiếp trực tiếp qua tập lệnh AT thông qua UART. 
* **Tương quan với svc_network:** `drv_a7680c` nằm ở dưới cùng. Toàn bộ phần quay số PPPoS, cấu hình APN, khóa mạng LTE (`AT+CNMP=38`) đều diễn ra ở tầng trên `svc_network`. Tầng trên chỉ gọi xuống driver này để thao tác chân phần cứng hoặc kích hoạt mode khẩn cấp.
