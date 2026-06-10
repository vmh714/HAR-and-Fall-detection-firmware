# 📐 Component: lib_kalman (1D Kalman Filter Library)

> **Mục đích**: Bộ lọc Kalman 1 chiều (1D) tối ưu để ước lượng góc nghiêng (Roll và Pitch) bằng cách dung hợp (sensor fusion) dữ liệu từ Gia tốc kế và Cảm biến góc quay.
> **Cập nhật cuối (Timestamp)**: 2026-05-21T15:07:00+07:00
> **Trạng thái**: Hoạt động ổn định

---

## 1. TẠI SAO CẦN BỘ LỌC KALMAN?
Khi tính toán góc nghiêng (Roll/Pitch) từ IMU:
*   **Gia tốc kế (Accelerometer)**: Tính toán góc tĩnh rất tốt, nhưng bị cực kỳ nhiễu khi thiết bị chuyển động đột ngột hoặc va đập (high-frequency noise).
*   **Cảm biến góc quay (Gyroscope)**: Phản ứng cực nhanh với chuyển động, không bị ảnh hưởng bởi va đập nhưng khi tích phân vận tốc góc theo thời gian ($dt$) sẽ sinh ra sai số tích lũy tăng dần theo thời gian (drift).

**Bộ lọc Kalman** giải quyết triệt để hai nhược điểm này bằng cách kết hợp ưu điểm của cả hai cảm biến:
1.  **Dự báo (Predict)**: Sử dụng vận tốc góc từ Gyroscope để cập nhật dự báo góc mới và sai số trôi (bias).
2.  **Cập nhật (Update)**: Sử dụng góc tính toán từ Gia tốc kế để hiệu chỉnh, triệt tiêu sai số trôi tích lũy từ Gyroscope.

```
       ┌───────────────────┐    Góc Dự báo
Gyros  │ Vận tốc góc * dt  │ ───────────────┐
       └───────────────────┘                ▼
                                   ┌────────────────┐    Góc sạch, mượt
                                   │  Kalman Filter │ ──────────────────> (Roll/Pitch)
       ┌───────────────────┐                ▲
Accel  │ Arctan(ax, ay, az)│ ───────────────┘
       └───────────────────┘     Góc Đo lường
```

---

## 2. THAM SỐ CẤU HÌNH BỘ LỌC (TUNING PARAMETERS)
Bộ lọc định nghĩa các tham số đặc trưng trong struct `kalman_t` để tinh chỉnh độ nhạy:

```c
typedef struct {
    float Q_angle;   // Phương sai nhiễu quá trình của góc (Mặc định: 0.001)
    float Q_bias;    // Phương sai nhiễu quá trình của sai số trôi (Mặc định: 0.003)
    float R_measure; // Phương sai nhiễu đo lường của gia tốc kế (Mặc định: 0.03)

    float angle;     // Góc ước lượng đầu ra (Roll/Pitch)
    float bias;      // Sai số trôi hiện tại của Gyroscope
    float P[2][2];   // Ma trận hiệp phương sai sai số ước lượng
} kalman_t;
```

### Ý nghĩa tinh chỉnh (Tuning Guide)
*   **`R_measure`**: Thể hiện mức độ tin cậy vào gia tốc kế.
    *   Nếu tăng `R_measure`: Bộ lọc sẽ ít tin tưởng vào Gia tốc kế hơn, tin tưởng Gyroscope nhiều hơn ➔ Góc đầu ra sẽ cực kỳ mượt mà, không bị rung giật khi va chạm nhưng phản ứng chậm hơn với sự thay đổi góc tĩnh thực tế.
    *   Trong bài toán té ngã, có cú va chạm mạnh (Impact), gia tốc kế bị nhiễu cục bộ cực cao, việc cấu hình `R_measure` cân bằng (~0.03) giúp giữ góc ước lượng ổn định tránh báo động giả.

---

## 3. HƯỚNG DẪN SỬ DỤNG API
Các hàm công bố trong [kalman_filter.h](file:///d:/datn/firmware/components/lib_kalman/include/kalman_filter.h):

### 3.1 Khởi tạo bộ lọc
```c
void kalman_init(kalman_t *kalman);
```
Đặt các tham số lọc về giá trị mặc định tối ưu đã qua kiểm thử thực tế và xóa ma trận hiệp phương sai sai số về 0.

### 3.2 Cập nhật góc liên tục
```c
float kalman_get_angle(kalman_t *kalman, float new_angle, float new_rate, float dt);
```
*   `new_angle`: Góc đo lường tính trực tiếp từ Gia tốc kế tại chu kỳ hiện tại (bằng hàm lượng giác `atan2`).
*   `new_rate`: Vận tốc góc quay từ Gyroscope (đã trừ đi sai số tĩnh offset).
*   `dt`: Thời gian lấy mẫu thực tế trôi qua giữa 2 chu kỳ (đơn vị giây, ví dụ $0.01s$ tương ứng $100Hz$).
*   **Đầu ra**: Góc đã lọc sạch nhiễu trôi và rung động.

---

## 4. VÍ DỤ MINH HỌA TÍCH HỢP TRONG CODE

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
### 3.3 Lọc Kalman 1D (Mới thêm Phase 4)
Để tiền xử lý dữ liệu đầu vào cho mảng TinyML, hệ thống cần làm phẳng tín hiệu của từng trục gia tốc và con quay hồi chuyển độc lập (tổng cộng 6 trục).
```c
typedef struct {
    float x;  // Giá trị ước lượng hiện tại
    float P;  // Hiệp phương sai sai số
    float Q;  // Nhiễu quá trình (càng lớn càng bám sát dữ liệu thực)
    float R;  // Nhiễu đo lường (càng lớn càng mượt nhưng bị trễ)
} kalman_1d_t;

void kalman_1d_init(kalman_1d_t *kf, float Q, float R, float initial_value);
float kalman_1d_update(kalman_1d_t *kf, float measurement);
```
Mỗi trục của IMU (ax, ay, az, gx, gy, gz) sẽ có một instance `kalman_1d_t` riêng để chuẩn hóa đầu vào model.
