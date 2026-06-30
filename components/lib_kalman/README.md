# 📐 Component: lib_kalman (1D Kalman Filter Library)

> **Cập nhật cuối (Timestamp):** 2026-06-30
> **Trạng thái:** Hoạt động ổn định

---

## 1. PUBLIC API & VÍ DỤ SỬ DỤNG

Các hàm công bố trong [kalman_filter.h](include/kalman_filter.h):

### 1.1 Khởi tạo và cập nhật góc Roll/Pitch (2 trạng thái)
```c
void kalman_init(kalman_t *kalman);
float kalman_get_angle(kalman_t *kalman, float new_angle, float new_rate, float dt);
```
- `kalman_init`: Đặt các tham số lọc về giá trị mặc định tối ưu đã qua kiểm thử thực tế và xóa ma trận hiệp phương sai sai số về 0.
- `kalman_get_angle`: Cập nhật góc liên tục dựa vào `new_angle` (tính từ Gia tốc kế), `new_rate` (từ Gyroscope), và chu kỳ `dt`.

### 1.2 Lọc Kalman 1D làm mượt tín hiệu (1 trạng thái)
Mỗi trục của IMU (ax, ay, az, gx, gy, gz) sẽ có một instance `kalman_1d_t` riêng để chuẩn hóa đầu vào model:
```c
void kalman_1d_init(kalman_1d_t *kf, float Q, float R, float initial_value);
float kalman_1d_update(kalman_1d_t *kf, float measurement);
```
`kalman_1d_init` đặt giá trị ước lượng ban đầu `x = initial_value` và hiệp phương sai sai số `P = 1.0` (uncertainty cao có chủ đích), giúp bộ lọc bám nhanh về tín hiệu thực trong vài chu kỳ đầu. 

### 1.3 Ví dụ tích hợp trong code
```c
#include "kalman_filter.h"
#include <math.h>

kalman_t filter_roll;
kalman_init(&filter_roll);

// Trong vòng lặp đọc dữ liệu 100Hz (dt = 0.01)
float accel_roll = atan2(accel_y, accel_z) * 180.0 / M_PI; // Tính góc thô từ gia tốc kế
float gyro_roll_rate = gyro_x;                            // Vận tốc góc roll từ gyro

// Lọc Kalman
float smooth_roll = kalman_get_angle(&filter_roll, accel_roll, gyro_roll_rate, 0.01f);
```

---

## 2. Mục đích (Purpose / Why)
Bộ lọc Kalman 1 chiều tối ưu để ước lượng góc nghiêng (Roll và Pitch) bằng cách dung hợp (sensor fusion) dữ liệu từ Gia tốc kế và Cảm biến góc quay.
Gia tốc kế tính góc tĩnh rất tốt, nhưng bị cực kỳ nhiễu khi thiết bị chuyển động đột ngột hoặc va đập (high-frequency noise). Ngược lại, Gyroscope phản ứng nhanh nhưng có sai số tích lũy (drift). Bộ lọc Kalman giải quyết bằng cách:
1.  **Dự báo (Predict)**: Sử dụng vận tốc góc từ Gyroscope để cập nhật dự báo góc mới và sai số trôi (bias).
2.  **Cập nhật (Update)**: Sử dụng góc tính toán từ Gia tốc kế để hiệu chỉnh, triệt tiêu sai số trôi tích lũy từ Gyroscope.

## 3. Cơ chế cốt lõi (How it works)

### 3.1 Tham số cấu hình (Tuning Parameters)
Bộ giá trị mặc định (`Q_angle = 0.005`, `Q_bias = 0.001`, `R_measure = 0.05`) được tinh chỉnh cho bài toán phát hiện té ngã:
*   **`Q_angle`** được nâng cao (so với mức thông thường) để bộ lọc phản ứng nhạy hơn với những thay đổi góc đột ngột khi té ngã.
*   **`Q_bias`** được giữ thấp vì Gyroscope đã được calibrate offset tĩnh.
*   **`R_measure`**: Thể hiện mức độ tin cậy vào gia tốc kế. Trong bài toán té ngã, có cú va chạm mạnh, việc cấu hình `R_measure` cân bằng (~0.05) giúp giữ góc ước lượng ổn định tránh báo động giả.

### 3.2 Cơ chế Fall Detection đặc thù trong Kalman 2 trạng thái
Hàm `kalman_get_angle` có bổ sung các xử lý đặc thù để phục vụ phát hiện té ngã:
1.  **Chuẩn hóa sai số góc về [-180°, 180°]**: Tránh hiện tượng bộ lọc hiểu sai một bước nhảy nhỏ qua biên $\pm 180°$ thành một thay đổi góc khổng lồ.
2.  **Cơ chế "jump-reset" khi $|y| > 90°$**: Khi người chuyển đột ngột từ tư thế **đứng sang nằm** lúc ngã, sai số góc vượt 90 độ. Bộ lọc sẽ không làm trơn từ từ mà **reset thẳng**: gán `angle = new_angle`, đặt `bias = 0` và reset ma trận $P$ về khởi đầu cao, rồi trả về ngay. Đây là mấu chốt giúp Kalman hội tụ cực nhanh về tư thế nằm, không bị trễ.
3.  **Chuẩn hóa góc đầu ra**: Đảm bảo Roll/Pitch luôn nằm trong dải $[-180°, 180°]$.

### 3.3 Bộ lọc 1D làm mượt (Smoothing)
Mô hình dự báo của hàm `kalman_1d_update` là *constant model* (`x_pred = x`, `P_pred = P + Q`), do đó đây là bộ lọc 1 trạng thái thuần làm phẳng tín hiệu, không có xử lý góc hay bias như cấu trúc 2 trạng thái. Nó giúp tiền xử lý khử nhiễu trực tiếp trên 6 trục IMU thô để làm đầu vào cho TinyML INT8.

## 4. Đa nhiệm & Tài nguyên (Concurrency & Resources)
* Thuần túy là thư viện tính toán toán học, không sinh bất kỳ Thread hay Task nào.
* Không có dynamic allocation (`malloc`). Việc tính toán sử dụng floating-point 32-bit (FPU) nhẹ nhàng cho ESP32-S3.

## 5. Luồng giao tiếp (Data Flow)
* Thư viện không tự động lấy dữ liệu. Module `svc_imu` là caller chịu trách nhiệm thu thập số đo từ `drv_mpu6050`, sau đó đẩy tham số vào hàm `kalman_get_angle` / `kalman_1d_update` và nhận lại giá trị đã lọc.
