# ☁️ Component: svc_cloud (Cloud Application & MQTT Service)

> **Cập nhật cuối (Timestamp):** 2026-06-30
> **Trạng thái:** Hoạt động ổn định (Tương thích tốt với WiFi và 4G/PPP)

---

## 1. PUBLIC API

Các hàm công bố trong [svc_cloud.h](include/svc_cloud.h):

### 1.1 Khởi tạo kết nối Đám mây
```c
esp_err_t svc_cloud_init(const char *broker_uri, const char *client_id, const char *username, const char *password);
```
*   **Mô tả**: Lưu thông tin cấu hình, nạp các tham số từ NVS, tạo hàng đợi `s_imu_queue` và đăng ký event handlers. Lệnh gọi sẽ tự khởi động task `svc_cloud_task` (Priority 5, stack 4KB).
*   **Cấu hình MQTT tối ưu (4G/PPP)**:
    - `session.keepalive = 45`: Nhanh chóng phát hiện đứt kết nối vô tuyến.
    - `network.timeout_ms = 15000`: Dung sai thời gian handshake TLS trên kết nối di động độ trễ cao.
    - `network.reconnect_timeout_ms = 8000`: Tần suất thử kết nối lại.
    - `task.priority = 6`: Đảm bảo task MQTT chạy ổn định không bị starve bởi các task non-realtime.

### 1.2 Kiểm tra kết nối MQTT
```c
bool svc_cloud_is_connected(void);
```

### 1.3 Đóng gói và Gửi dữ liệu (Publish)
```c
int svc_cloud_publish(const char *topic, const char *data, int qos, int retain);
```

### 1.4 Đẩy gói dữ liệu IMU vào hàng đợi (Thread-Safe Enqueue)
```c
esp_err_t svc_cloud_enqueue_imu_batch(const void *batch_data);
```
Nhận con trỏ gói batch dữ liệu, gửi vào hàng đợi tĩnh `s_imu_queue` với timeout bằng 0 (fail-fast, không block task IMU 100Hz). Trả về `ESP_ERR_NO_MEM` nếu hàng đợi đầy.

### 1.5 Lấy giá trị cấu hình timeout
```c
uint32_t svc_cloud_get_stream_timeout(void);
```
Trả về thời gian giới hạn tự động dừng stream (đơn vị phút, lưu trong NVS `str_to`, mặc định 5 phút). Thường được `sys_manager` gọi để nạp timer tự stop.

---

## 2. Mục đích (Purpose / Why)
Chịu trách nhiệm quản lý tầng ứng dụng giao tiếp với đám mây. Quản lý vòng đời kết nối MQTT Client, xử lý đăng ký (subscribe) để nhận lệnh cấu hình, và xuất bản (publish) các luồng dữ liệu thô / trạng thái thiết bị lên Cloud thông qua Wifi hoặc 4G PPPoS.

## 3. Giao thức truyền thông MQTT (Topics & Payloads)
Giao tiếp thông qua giao thức MQTT với các Topic chuẩn hóa. Khóa định danh thiết bị trong đường topic là `{mac}` (chuỗi Hex địa chỉ MAC của chip WiFi STA lấy từ eFuse, ví dụ `240ac4...`), giúp tự động phân biệt thiết bị mà không cần hardcode ID (D-020).

### 3.1 Các Topic Chính
Tất cả topic dùng prefix `eldercare/`.
*   **`eldercare/{mac}/status`** (Publish, QoS 0): Gửi dữ liệu telemetry/trạng thái thiết bị định kỳ (mặc định 5s, lưu ở NVS `tel_int`). Chỉ gửi khi FSM ở `STATE_NORMAL`.
*   **`eldercare/{mac}/config/status`** (Publish, QoS 1): Báo cáo trạng thái cấu hình hiện tại (interval, fall_threshold, fall_cooldown, stream_timeout, fall_confirm_window, rssi_interval, fw_version). Tự động gửi lúc kết nối/kết nối lại và sau mỗi lần nhận cấu hình thành công.
*   **`eldercare/{mac}/alert/fall`** (Publish, QoS 1): Gửi thông điệp cảnh báo té ngã.
*   **`eldercare/{mac}/imu_stream`** (Publish, QoS 0): Gửi luồng IMU raw 100Hz dưới dạng gom nhóm Base64 khi ở `STATE_STREAMING`.
*   **`eldercare/{mac}/command`** (Subscribe, QoS 1): Lắng nghe lệnh điều khiển từ Backend.
*   **`eldercare/{mac}/config/set`** (Subscribe, QoS 1): Nhận cấu hình từ Backend.

### 3.2 Định dạng dữ liệu mẫu (JSON Payloads)
*   **Status Payload** (topic `status`):
    ```json
    {
      "battery": 98,
      "walk_steps": 5,
      "run_steps": 0,
      "steps": 5,
      "state": "NORMAL",
      "ai_pred": "Idle",
      "ai_conf": 0.98,
      "rssi": -65
    }
    ```
    - `battery`: Phần trăm pin đọc từ driver `drv_battery`.
    - `walk_steps` / `run_steps`: Số bước đi bộ / chạy bộ tăng thêm (delta) trong chu kỳ này.
    - `steps`: Tổng số bước tăng thêm (`walk_steps + run_steps`).
    - `ai_pred` / `ai_conf`: Dự đoán lớp hành động và độ tin cậy tương ứng từ `svc_ai`.

*   **Fall Alert Payload** (topic `alert/fall`):
    ```json
    {
      "user_name": "",
      "message": "Fall detected",
      "confidence": 0.85
    }
    ```

*   **Raw IMU Telemetry** (topic `imu_stream`):
    ```json
    {
      "ts": 12345678,
      "seq": 102,
      "fs": 100,
      "cnt": 50,
      "data_b64": "AQAeAP7/AAD+////..."
    }
    ```
    - `seq`: Số thứ tự tăng đơn điệu của batch giúp Frontend xác định mất mát mẫu.
    - `data_b64`: Mảng 50 mẫu `imu_stream_data_t` (đã lọc Kalman & đổi hệ trục thắt lưng) mã hóa Base64.

## 4. Cơ chế cốt lõi (How it works)
`svc_cloud` hoạt động theo mô hình **Producer-Consumer** phi luồng (thread-safe) kết hợp với hàng đợi sự kiện động để bảo vệ an toàn cho bộ nhớ hệ thống:
1. **MQTT Lifecycle**: Trực tiếp lắng nghe sự kiện mạng (`NET_EVENT`). Khi mạng kết nối (`NET_EVT_WIFI_CONNECTED` hoặc `NET_EVT_CELLULAR_CONNECTED`), module tự động cấu hình và khởi chạy MQTT Client trong background. Khi mất mạng, Client tự động dừng.
2. **Cảnh báo ngã (`ai_event_handler`)**: Lắng nghe `AI_EVT_FALL_DETECTED` từ `svc_ai` và publish cảnh báo SOS lên topic `alert/fall` (QoS 1). Có cơ chế **cooldown cố định 15 giây** (`s_fall_cooldown_us`, lưu trong NVS `fall_cd`) để chống spam alert lặp.
3. **Watchdog Tự Phục Hồi MQTT**:
    - **Bậc 1 (~60s)**: Nếu mất kết nối MQTT client nhưng IP vẫn thông (mạng vẫn up), hệ thống sẽ dừng và khởi chạy lại MQTT client (`stop/start`) để làm sạch socket/handshake TLS.
    - **Bậc 2 (~150s)**: Nếu kẹt quá lâu không khôi phục, hệ thống thực hiện ghi bản tin ngã chưa gửi (nếu có) vào NVS và gọi `esp_restart()` để reset nóng cả MCU, dọn dẹp các treo ngầm của module A7680C ở tầng dưới (D-024).
4. **Cờ comms-critical & Hỗ trợ phục hồi**: 
    - Khi có sự kiện ngã, `svc_cloud` sẽ gia hạn trạng thái comms-critical thông qua `sys_manager_bump_comms_critical` để ngăn chặn dịch vụ mạng ngắt kết nối đo RSSI giữa chừng (D-022).
    - Hỗ trợ lưu trữ Alert vào NVS (`unsent_al`) trước khi khởi động lại do lỗi mạng và tự động gửi lại (re-publish) ngay khi kết nối lại MQTT thành công.

## 5. Đa nhiệm & Tài nguyên (Concurrency & Resources)
* **Background Worker Task (`svc_cloud_task`)**: Chạy vòng lặp vô tận chờ dữ liệu từ `s_imu_queue`. Khi có dữ liệu (ở `STATE_STREAMING`), task tiến hành Base64 mã hóa mảng nhị phân thô, đóng gói JSON và thực hiện `esp_mqtt_client_publish` lên đám mây. Việc xử lý nặng này chạy ở background và hoàn toàn cách ly khỏi System Event Loop.
* **Hàng đợi Dữ liệu (`s_imu_queue`)**: Khởi tạo hàng đợi tĩnh có dung lượng chứa tối đa 5 gói `imu_batch_data_t` (tức 5 x 50 mẫu).

## 6. Luồng giao tiếp và Kiểm soát dòng chảy (Flow Control)
- **QoS 0 cho Telemetry & Stream**: Telemetry và Stream IMU raw có tần suất cao, việc mất vài gói tin được chấp nhận để đảm bảo tốc độ truyền và không làm nghẽn hệ thống.
- **QoS 1 cho Alert & Config**: Cảnh báo ngã và Cấu hình bắt buộc phải đến đích. Alert sử dụng RAM cache và NVS cache để đảm bảo không bị thất lạc khi mất mạng tạm thời.
- **Data Drop Policy (D-025)**: Khi mất kết nối MQTT, dịch vụ sẽ lập tức hủy bỏ (drop) các gói IMU batch nhận được thay vì tích lũy trong hàng đợi, tránh tràn bộ nhớ RAM. Cú nhảy thời gian (gap) này sẽ được Frontend/Backend tự động nhận diện thông qua timestamp (`Δts > 500ms` giữa hai batch liên tiếp) để cảnh báo chất lượng bản ghi dữ liệu (D-025).
- **Auto-stop Gating**: Nếu lệnh publish một batch IMU raw tốn quá 500ms (do mạng 4G nghẽn hoặc mất gói), `svc_cloud` sẽ chủ động phát event `CLOUD_CMD_STOP_STREAM` để đưa hệ thống về `STATE_NORMAL` nhằm tránh tràn bộ đệm cảm biến phần cứng (D-025).
