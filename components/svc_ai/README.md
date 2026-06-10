# Component: svc_ai

## Tổng Quan
Dịch vụ xử lý nghiệp vụ AI (AI Service) đóng vai trò điều phối giữa luồng dữ liệu (IMU) và nhân xử lý Tensor (TinyML). Vận hành bằng một FreeRTOS Task độc lập.

## Public API
- `esp_err_t svc_ai_init(void)`: Khởi tạo Queue nhận dữ liệu, cấp phát tài nguyên và chạy Task `ai_processing_task`. Gọi `tflite_init()` ngầm bên trong.
- `void svc_ai_process_window(const imu_window_t *window)`: API được gọi từ `svc_imu` để copy mảng Sliding Window vào Queue của AI Task một cách bất đồng bộ.

## Cơ chế hoạt động (Workflow)
1. Task `svc_ai` nằm chờ ở hàm `xQueueReceive`.
2. Khi có dữ liệu Sliding Window, Task thức dậy, gọi API của `lib_tinyml` để suy luận.
3. **Posture Logic**: Nếu hành vi AI phán đoán là `Idle`, Task AI sẽ check góc `Pitch` (từ `imu_service_get_latest_pitch`) để suy ra tư thế (Đứng/Ngồi hay Đang Nằm).
4. **Cảnh Báo**: Nếu `fall_prob >= 25%`, nó sẽ phát đi Event `AI_EVT_FALL_DETECTED` lên `System Event Loop`.
