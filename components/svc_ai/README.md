# Component: svc_ai (AI Inference & Post-Impact Confirmation Service)

> **Cập nhật cuối (Timestamp):** 2026-06-30
> **Trạng thái:** Hoạt động ổn định

---

## 1. PUBLIC API

- **`esp_err_t svc_ai_init(void)`**: Tạo Queue nhận dữ liệu (depth 2) và khởi chạy Task `svc_ai_task` (stack 6KB, priority 6). Bộ thông dịch TFLite (`tflite_init()`) được khởi tạo tự động khi task bắt đầu.
- **`void svc_ai_process_window(const imu_window_t *window)`**: Nhận con trỏ Sliding Window 200 mẫu từ `svc_imu` và đẩy vào Queue. Hàm chạy non-blocking (timeout 0) để không block task 100Hz của IMU.
- **`const char* svc_ai_get_latest_prediction(void)`**: Lấy chuỗi text của nhãn dự đoán gần nhất (`"Walk"`, `"Run"`, `"Idle"`, `"Trans"`, `"Fall"`).
- **`float svc_ai_get_latest_confidence(void)`**: Lấy độ tin cậy (từ 0.0 đến 1.0) của dự đoán gần nhất.
- **`void svc_ai_set_confirm_window_ms(uint32_t ms)` / `get`**: Thiết lập và đọc thời gian của cửa sổ xác nhận ngã (lưu trong NVS `fall_cf`, mặc định 4000ms).

---

## 2. Mục đích (Purpose / Why)
Dịch vụ xử lý suy luận AI (AI Service) đóng vai trò điều phối giữa luồng dữ liệu cảm biến (Sliding Window từ `svc_imu`) và nhân xử lý tensor TensorFlow Lite Micro (`lib_tinyml`). 
Nó không chỉ làm nhiệm vụ gọi model để lấy kết quả (Inference), mà quan trọng hơn là đóng vai trò **Bộ lọc thông minh (Post-Impact Confirmation FSM)** nhằm giảm thiểu tối đa các báo động giả (ngồi phịch, nhảy, va chạm mạnh rồi đứng dậy) trong thực tế (D-021).

## 3. Cơ chế cốt lõi (How it works): FSM Xác Nhận Ngã 2 Pha
Nhằm duy trì recall cực cao của mô hình AI nhưng hạn chế tối đa báo động giả, hệ thống áp dụng cơ chế xác nhận 2 pha (D-021):

```text
                     ┌──────────────────┐
                     │ FALL_FSM_NORMAL  │
                     └──────────────────┘
                               │
                               │ [Pha 1] ML Trigger (predicted == Fall)
                               ▼
                    ┌─────────────────────┐
                    │ FALL_FSM_CONFIRMING │
                    └─────────────────────┘
                               │
             ┌─────────────────┴─────────────────┐
             ▼ (Hồi phục)                        ▼ (Hết cửa sổ xác nhận)
   [WALK/RUN] + Upright                  Kiểm tra tỷ lệ Lying + Idle
             │                                   │
             ▼                                   ├─► >= 60% : CONFIRMED! Phát AI_EVT_FALL_DETECTED
    (Huỷ bỏ cảnh báo)                            │
             │                                   └─► < 60%  : Huỷ bỏ, về NORMAL
             ▼                                   
    [FALL_FSM_NORMAL]                            
```

1. **Pha 1: ML Trigger**: Khi model TFLite phán đoán lớp `Fall` (ngưỡng `fall_threshold` lưu ở NVS `fall_thr`, mặc định 0.25), FSM chuyển từ `NORMAL` sang `CONFIRMING` và bắt đầu tính giờ `fall_confirm_window` (mặc định 4s, D-021). Đồng thời, hệ thống dựng cờ `comms-critical` (D-022) để giữ kết nối mạng ổn định không bị đứt do đo RSSI.
2. **Pha 2: Quan sát Post-Impact**: Trong cửa sổ `CONFIRMING`:
    - Nếu người dùng đứng dậy và di chuyển (nhận diện lớp `Walk`/`Run` kèm tư thế đứng `is_stand_sit = true`), FSM lập tức **huỷ bỏ (abort)** và quay về `NORMAL`.
    - Thiết bị liên tục theo dõi tư thế thông qua góc **Roll** dung hợp Kalman (`imu_service_get_latest_roll()`). Tư thế đứng/ngồi (Upright) được định nghĩa khi góc Roll nằm trong khoảng `[60, 90]` hoặc `[-90, -60]` độ (thắt lưng front-mounting, D-013). Nếu nằm ngoài khoảng này, thiết bị được coi là nằm (`is_lying = true`).
    - Nếu cửa sổ thời gian kết thúc, FSM kiểm tra tỷ lệ số window thỏa mãn điều kiện (`Idle` + nằm). Nếu tỷ lệ này **`>= 60%`** → **Xác nhận ngã (Confirmed)** và phát đi sự kiện `AI_EVT_FALL_DETECTED`. Ngược lại, huỷ bỏ cảnh báo.

## 4. Đa nhiệm & Tài nguyên (Concurrency & Resources)
* Dịch vụ chạy trong một FreeRTOS Task độc lập (`svc_ai_task`, stack 6KB, priority 6). Do việc tính toán Tensor Ops khá tốn CPU, mức priority 6 đảm bảo nó không cản trở việc đọc MPU6050 (priority 10) nhưng vẫn ưu tiên hơn việc gửi MQTT (priority 5).
* **Phân luồng hoạt động (FSM Gating):** Theo thiết kế gating hệ thống (D-024), **suy luận AI chỉ chạy ở `STATE_NORMAL`**. Khi ở `STATE_STREAMING` (thu dữ liệu thô), `svc_imu` sẽ ngừng gửi sliding window sang `svc_ai` để CPU tập trung cho việc truyền dữ liệu.

## 5. Luồng giao tiếp (Data Flow)
* **Input**: Nhận bản sao chép của `imu_window_t` (200 mẫu x 6 trục) từ `svc_imu` qua FreeRTOS Queue (depth = 2) mỗi khi có 50 mẫu mới.
* **Output / Event**: Nếu FSM xác nhận chắc chắn có người ngã (CONFIRMED), dịch vụ sử dụng cơ chế `esp_event_post()` phát đi sự kiện `AI_EVT_FALL_DETECTED`. Sự kiện này sẽ được `svc_cloud` và `sys_manager` lắng nghe ở background để gửi cảnh báo SOS.
