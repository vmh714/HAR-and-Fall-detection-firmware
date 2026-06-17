# Component: svc_ai

## Tổng Quan
Dịch vụ xử lý nghiệp vụ AI (AI Service) đóng vai trò điều phối giữa luồng dữ liệu (IMU) và nhân xử lý Tensor (TinyML). Vận hành bằng một FreeRTOS Task độc lập.

## Public API
- `esp_err_t svc_ai_init(void)`: Tạo Queue nhận dữ liệu (depth 2) và chạy Task `svc_ai_task` (stack 6144, priority 6). `tflite_init()` được gọi ngầm bên trong task khi task khởi động.
- `void svc_ai_process_window(const imu_window_t *window)`: API được gọi từ `svc_imu` để đẩy **con trỏ** Sliding Window vào Queue của AI Task. Dùng timeout 0 (non-blocking) để không chặn luồng gọi (luồng `svc_imu`); chỉ con trỏ được copy, không copy toàn bộ dữ liệu window.
- `const char* svc_ai_get_latest_prediction(void)`: Trả về chuỗi text của kết quả dự đoán gần nhất (ví dụ "Walk", "Run", "Idle", "Trans", "Fall", hoặc "Unknown").
- `float svc_ai_get_latest_confidence(void)`: Trả về độ tự tin (max_prob, từ 0.0 đến 1.0) của kết quả dự đoán gần nhất.

## Cơ chế hoạt động (Workflow)
1. Task `svc_ai` nằm chờ ở hàm `xQueueReceive` (timeout `portMAX_DELAY`).
2. **Tiền xử lý (Flatten & Interleave)**: Khi nhận được con trỏ window, Task chuyển dữ liệu từ dạng SoA (Structure of Arrays — `win->ax[]`, `win->ay[]`, ...) sang một mảng 1D xen kẽ (interleaved) gồm `200 × 6 = 1200` phần tử float theo thứ tự `ax, ay, az, gx, gy, gz`. Vì đây là Ring Buffer, Task duyệt vòng tròn từ `head` (mẫu cũ nhất) tới `head + 200`. Kích thước window là 200 mẫu (`IMU_WINDOW_SIZE`).
3. **Inference**: Task gọi `tflite_run_inference_with_data()` của `lib_tinyml`. Hàm này chịu trách nhiệm Quantization (float → int8), `Invoke()` và Thresholding.
4. **Posture Logic**: Nếu hành vi AI phán đoán là `Idle` (`AI_CLASS_IDLE`), Task lấy góc `Pitch` qua `imu_service_get_latest_pitch()` để suy ra tư thế: Pitch trong khoảng `[-45, 45]` độ → Đứng/Ngồi (Stand/Sit), ngoài khoảng → Đang Nằm (Lying Down).
5. **Cảnh Báo**: Việc thresholding té ngã nằm ở `lib_tinyml` (`tflite_wrapper.cpp`), KHÔNG ở `svc_ai`. Khi `lib_tinyml` gán `predicted_class = Fall` (điều kiện `fall_prob >= 60%`, tức `0.6`), `svc_ai` chỉ kiểm tra `result.predicted_class == AI_CLASS_FALL` rồi phát Event `AI_EVT_FALL_DETECTED` trên base `AI_EVENT` lên System Event Loop.

## Lớp phân loại (Classes)
Danh sách lớp dùng trong code: `{"Walk", "Run", "Idle", "Trans", "Fall"}`.

## Ghi chú về Cooldown (chống spam)
`svc_ai` **không** có logic cooldown — nó phát Event `AI_EVT_FALL_DETECTED` mỗi lần phát hiện ngã. Việc chống spam được đảm nhiệm bởi `svc_cloud`: một cooldown cố định **15 giây** (`FALL_COOLDOWN_US`, xem `svc_cloud.c`) lọc các event trùng lặp trước khi publish SOS alert qua MQTT.

## Tham chiếu
- Header: `include/svc_ai.h`
- Thresholding ngã: `../lib_tinyml/tflite_wrapper.cpp`
- Cooldown publish: `../svc_cloud/svc_cloud.c`

> **Cập nhật cuối (Timestamp):** 2026-06-17
