# 🌐 Component: svc_network (WiFi & MQTT Network Service)

> **Mục đích**: Chịu trách nhiệm quản lý kết nối không dây WiFi Station (STA), quản lý vòng đời Client MQTT, nhận lệnh điều khiển từ xa và truyền thông tin trạng thái lên máy chủ đám mây.
> **Cập nhật cuối (Timestamp)**: 2026-05-21T15:07:00+07:00
> **Trạng thái**: Hoạt động ổn định (Sẵn sàng mở rộng sang 4G LTE ở Phase 2)

---

## 1. NGUYÊN LÝ HOẠT ĐỘNG & BỘ SỰ KIỆN MẠNG
Dịch vụ mạng hoạt động theo mô hình bất đồng bộ hướng sự kiện, tự động thử kết nối lại khi mất sóng vật lý hoặc mất kết nối Broker.

### 1.1 Khởi tạo mạng song song (Parallel Boot Sequence)
Để giảm thiểu thời gian khởi động thiết bị (Boot Time), quá trình kết nối mạng WiFi chạy song song với quá trình hiệu chuẩn IMU cảm biến:
1.  `app_main` khởi tạo nền tảng Netif và Event Loop chung.
2.  Nhánh 1: IMU bắt đầu I2C Init và Calibrate (~6 giây tĩnh).
3.  Nhánh 2: `svc_network` khởi tạo WiFi STA, kết nối bộ thu phát sóng RF và đợi cấp IP.
4.  Cả hai nhánh đồng bộ hóa tại Sync Barrier. Luồng truyền dữ liệu chỉ bắt đầu khi cả 2 đã sẵn sàng.

---

## 2. GIAO THỨC TRUYỀN THÔNG MQTT (TOPICS & PAYLOADS)

Hệ thống giao tiếp thông qua giao thức MQTT với các Topic chuẩn hóa sau:

### 2.1 Các Topic Chính
*   **`v1/devices/{device_id}/status`** (Publish, định kỳ 1 phút/lần hoặc khi đổi trạng thái FSM): Gửi dữ liệu trạng thái pin, trạng thái hoạt động cục bộ.
*   **`v1/devices/{device_id}/event`** (Publish, tức thời): Gửi thông báo chuyển đổi trạng thái đặc biệt.
*   **`v1/devices/{device_id}/alert/fall`** (Publish, khẩn cấp): Gửi thông điệp cảnh báo té ngã kèm độ tin cậy để dashboard hiển thị khẩn cấp (Modal + Còi hú).
*   **`v1/devices/{device_id}/telemetry/imu`** (Publish, 1Hz ở chế độ STREAMING): Gửi mảng JSON 50 mẫu raw phục vụ thu thập data.
*   **`v1/devices/{device_id}/cmd`** (Subscribe, lắng nghe lệnh từ Cloud): Nhận các lệnh cấu hình hoặc điều khiển.

### 2.2 Định dạng dữ liệu mẫu (JSON Payloads)
*   **Status Payload**:
    ```json
    {
      "battery": 87,
      "state": "STATE_NORMAL",
      "uptime_s": 320
    }
    ```
*   **Raw IMU Telemetry (Batch Mode)**:
    ```json
    {
      "device_id": "esp32s3_fall_01",
      "count": 50,
      "samples": [
        [0.01, 0.98, -0.05, 0.1, -0.2, 0.05],
        [0.02, 0.97, -0.04, 0.1, -0.1, 0.04]
      ]
    }
    ```

---

## 3. HƯỚNG DẪN SỬ DỤNG API
Các hàm công bố trong [wifi_mqtt_service.h](file:///d:/datn/firmware/components/svc_network/include/wifi_mqtt_service.h):

### 3.1 Khởi tạo dịch vụ mạng (Blocking Init)
```c
esp_err_t wifi_mqtt_service_init(const wifi_mqtt_config_t *config);
```
*   **Mô tả**: Thiết lập WiFi STA, đợi cho tới khi nhận địa chỉ IP thành công. Sau đó khởi chạy MQTT Client kết nối tới broker.
*   **Điều kiện tiên quyết**: Caller bắt buộc phải gọi `nvs_flash_init()`, `esp_netif_init()`, và `esp_event_loop_create_default()` trước khi gọi hàm này.

### 3.2 Kiểm tra trạng thái và Gửi dữ liệu
```c
bool wifi_mqtt_is_connected(void);
int wifi_mqtt_publish(const char *topic, const char *data, int qos, int retain);
```
*   `wifi_mqtt_publish` trả về ID gói tin gửi đi nếu thành công, hoặc `-1` nếu lỗi/mất kết nối mạng.

### 3.3 Lấy định danh thiết bị
```c
const char *wifi_mqtt_get_device_id(void);
```
Trả về chuỗi Device ID duy nhất (sinh ra từ MAC address vật lý của chip) để làm Client ID và điền vào các MQTT topic.

---

## 4. KẾ HOẠCH TIẾN HÓA MẠNG: WIFI ➔ 4G LTE A7680C (PHASE 2)
Để thiết bị hoạt động độc lập ngoài trời (Wearable), hệ thống mạng sẽ chuyển đổi từ WiFi sang 4G LTE:
1.  **Tách lớp Abstraction**: Tạo component `network_service` làm vỏ bọc chung. Các API giữ nguyên không đổi (`network_publish()`, `network_init()`).
2.  **Tích hợp Driver A7680C**: Dùng giao tiếp UART gửi lệnh AT command hoặc cấu hình PPPoS (Point-to-Point Protocol over Serial) thông qua component `esp_modem` của ESP-IDF.
3.  **Tái sử dụng mã nguồn**: Việc dùng PPPoS giúp ESP32-S3 có IP trực tiếp từ nhà mạng di động, cho phép giữ nguyên 100% mã nguồn kết nối MQTT hiện tại của `svc_network` mà không cần sửa đổi API truyền nhận đám mây.
---
