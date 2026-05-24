# ☁️ Component: svc_cloud (Cloud Application & MQTT Service)

> **Mục đích**: Chịu trách nhiệm quản lý tầng ứng dụng giao tiếp với đám mây. Quản lý vòng đời kết nối MQTT Client, xử lý đăng ký (subscribe) để nhận lệnh điều khiển, và xuất bản (publish) các luồng dữ liệu thô / trạng thái thiết bị lên Cloud.
> **Cập nhật cuối (Timestamp)**: 2026-05-21T16:04:00+07:00
> **Trạng thái**: Hoạt động ổn định (Sử dụng độc lập với tầng vật lý)

---

## 1. NGUYÊN LÝ HOẠT ĐỘNG
`svc_cloud` hoạt động theo mô hình **Producer-Consumer** phi luồng (thread-safe) kết hợp với hàng đợi sự kiện động để bảo vệ an toàn cho bộ nhớ hệ thống:
1. **MQTT Lifecycle**: Trực tiếp lắng nghe sự kiện mạng (`NET_EVENT`). Khi mạng kết nối (`NET_EVT_WIFI_CONNECTED` hoặc `NET_EVT_CELLULAR_CONNECTED`), module tự động cấu hình và khởi chạy MQTT Client trong background. Khi mất mạng, Client tự động dừng.
2. **Hàng đợi Dữ liệu (`s_imu_queue`)**: Khởi tạo hàng đợi tĩnh có dung lượng chứa tối đa 5 gói `imu_batch_data_t`.
3. **Background Worker Task (`svc_cloud_task`)**: Chạy vòng lặp vô tận chờ dữ liệu từ `s_imu_queue`. Khi có dữ liệu, task tiến hành Base64 mã hóa mảng nhị phân thô, đóng gói JSON và thực hiện `esp_mqtt_client_publish` lên đám mây. Việc xử lý nặng này chạy ở background và hoàn toàn cách ly khỏi System Event Loop.

---

## 2. GIAO THỨC TRUYỀN THÔNG MQTT (TOPICS & PAYLOADS)
Giao tiếp thông qua giao thức MQTT với các Topic chuẩn hóa. Tham số `{device_id}` được truyền vào trong quá trình khởi tạo.

### 2.1 Các Topic Chính
*   **`v1/devices/{device_id}/status`** (Publish): Gửi dữ liệu trạng thái pin, trạng thái hệ thống. Định kỳ hoặc khi đổi trạng thái FSM.
*   **`v1/devices/{device_id}/event`** (Publish): Gửi thông báo chuyển đổi trạng thái hoặc lỗi nghiêm trọng.
*   **`v1/devices/{device_id}/alert/fall`** (Publish): Gửi thông điệp cảnh báo té ngã (Post-impact fall detection alert).
*   **`v1/devices/{device_id}/telemetry/imu`** (Publish): Ở chế độ thu thập dữ liệu (Phase 1), gửi luồng data 100Hz dưới dạng gom nhóm (Batch 50 mẫu JSON).
*   **`v1/devices/{device_id}/cmd`** (Subscribe): Lắng nghe lệnh điều khiển hoặc cấu hình từ Cloud.

### 2.2 Định dạng dữ liệu mẫu (JSON Payloads)
*   **Status Payload**:
    ```json
    {
      "battery": 87,
      "state": "STATE_NORMAL",
      "uptime_s": 320
    }
    ```
*   **Raw IMU Telemetry (Base64 Encoded Batch)**:
    ```json
    {
      "ts": 12345678,
      "fs": 100,
      "cnt": 50,
      "data_b64": "AQAeAP7/AAD+////..."
    }
    ```
    - Trong đó:
      - `ts`: Timestamp của gói tin lấy từ timer hệ thống (ms).
      - `fs`: Tần số lấy mẫu (mặc định 100Hz).
      - `cnt`: Số lượng mẫu trong gói (mặc định 50 mẫu).
      - `data_b64`: Chuỗi nhị phân thô của mảng 50 mẫu `imu_stream_data_t` được mã hóa Base64 giúp tiết kiệm đến 50% dung lượng băng thông so với lưu mảng số JSON dạng text.

---

## 3. HƯỚNG DẪN SỬ DỤNG API
Các hàm công bố trong [svc_cloud.h](file:///d:/datn/firmware/components/svc_cloud/include/svc_cloud.h):

### 3.1 Khởi tạo kết nối Đám mây
```c
esp_err_t svc_cloud_init(const char *broker_uri, const char *client_id, const char *username, const char *password);
```
*   **Mô tả**: Khởi tạo ESP-MQTT Client với URI và thông tin chứng thực tương ứng. Nó sẽ thiết lập callback để tự động xử lý kết nối, tự động đăng ký (subscribe) các topic cấu hình sau khi kết nối.

### 3.2 Kiểm tra kết nối MQTT
```c
bool svc_cloud_is_connected(void);
```
*   Trả về `true` nếu thiết bị hiện đang kết nối trực tiếp với Broker MQTT.

### 3.3 Đóng gói và Gửi dữ liệu (Publish)
```c
int svc_cloud_publish(const char *topic, const char *data, int qos, int retain);
```
*   **Mô tả**: Đẩy chuỗi dữ liệu (JSON) lên topic chỉ định. Hàm này trừu tượng hóa lệnh gọi của esp_mqtt_client.
*   **Trả về**: Message ID nếu gửi thành công. Trả về `-1` nếu đang mất kết nối.

### 3.4 Đẩy gói dữ liệu IMU vào hàng đợi (Thread-Safe Enqueue)
```c
esp_err_t svc_cloud_enqueue_imu_batch(const void *batch_data);
```
*   **Mô tả**: Nhận gói `imu_batch_data_t` và đẩy vào hàng đợi tĩnh `s_imu_queue` để `svc_cloud_task` xử lý bất đồng bộ.
*   **Trả về**: `ESP_OK` nếu đưa vào queue thành công. Trả về `ESP_ERR_NO_MEM` nếu hàng đợi đầy.
