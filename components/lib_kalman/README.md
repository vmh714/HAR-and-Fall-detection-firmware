# 📐 Component: lib_kalman (1D Kalman Filter Library)

> **Mục đích**: Bộ lọc Kalman 1 chiều (1D) tối ưu để ước lượng góc nghiêng (Roll và Pitch) bằng cách dung hợp (sensor fusion) dữ liệu từ Gia tốc kế và Cảm biến góc quay.
> **Cập nhật cuối (Timestamp)**: 2026-06-17
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
                                   └────────────────┘
       ┌───────────────────┐                ▲
Accel  │ Arctan(ax, ay, az)│ ───────────────┘
       └───────────────────┘     Góc Đo lường
```

---

## 2. THAM SỐ CẤU HÌNH BỘ LỌC (TUNING PARAMETERS)
Bộ lọc định nghĩa các tham số đặc trưng trong struct `kalman_t` để tinh chỉnh độ nhạy:

```c
typedef struct {
    float Q_angle;   // Phương sai nhiễu quá trình của góc (Mặc định: 0.005)
    float Q_bias;    // Phương sai nhiễu quá trình của sai số trôi (Mặc định: 0.001)
    float R_measure; // Phương sai nhiễu đo lường của gia tốc kế (Mặc định: 0.05)

    float angle;     // Góc ước lượng đầu ra (Roll/Pitch)
    float bias;      // Sai số trôi hiện tại của Gyroscope
    float P[2][2];   // Ma trận hiệp phương sai sai số ước lượng
} kalman_t;
```

### Ý nghĩa tinh chỉnh (Tuning Guide)
Bộ giá trị mặc định (`Q_angle = 0.005`, `Q_bias = 0.001`, `R_measure = 0.05`) đã được tinh chỉnh riêng cho bài toán phát hiện té ngã:
*   **`Q_angle`** được nâng cao (so với mức thông thường) để bộ lọc phản ứng nhạy hơn với những thay đổi góc đột ngột khi té ngã.
*   **`Q_bias`** được giữ thấp vì Gyroscope đã được calibrate offset tĩnh, nên sai số trôi (bias) ổn định và không cần phương sai quá trình lớn.
*   **`R_measure`**: Thể hiện mức độ tin cậy vào gia tốc kế.
    *   Nếu tăng `R_measure`: Bộ lọc sẽ ít tin tưởng vào Gia tốc kế hơn, tin tưởng Gyroscope nhiều hơn ➔ Góc đầu ra sẽ cực kỳ mượt mà, không bị rung giật khi va chạm nhưng phản ứng chậm hơn với sự thay đổi góc tĩnh thực tế.
    *   Trong bài toán té ngã, có cú va chạm mạnh (Impact), gia tốc kế bị nhiễu cục bộ cực cao, việc cấu hình `R_measure` cân bằng (~0.05) giúp giữ góc ước lượng ổn định tránh báo động giả.

---

## 3. HƯỚNG DẪN SỬ DỤNG API
Các hàm công bố trong [kalman_filter.h](include/kalman_filter.h):

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

#### Cơ chế cốt lõi bên trong (Fall Detection)
Ngoài chu trình Kalman tiêu chuẩn (Predict ➔ tính Kalman Gain ➔ Update trạng thái và ma trận hiệp phương sai), hàm `kalman_get_angle` được bổ sung ba xử lý đặc thù để phục vụ phát hiện té ngã:

1.  **Chuẩn hóa sai số góc về [-180°, 180°]**: Sai số đo lường $y = \text{new\_angle} - \text{angle}$ được "wrap-around": nếu $y > 180°$ thì trừ đi $360°$, nếu $y < -180°$ thì cộng thêm $360°$. Việc này tránh hiện tượng bộ lọc hiểu sai một bước nhảy nhỏ qua biên $\pm 180°$ thành một thay đổi góc khổng lồ.

2.  **Cơ chế "jump-reset" khi $|y| > 90°$**: Khi sai số góc vượt ngưỡng $90°$ (ví dụ người chuyển đột ngột từ tư thế **đứng sang nằm** lúc ngã), bộ lọc coi đây là một bước nhảy hợp lệ cần bám theo ngay lập tức thay vì làm trơn từ từ. Khi đó bộ lọc **reset thẳng**: gán `angle = new_angle`, đặt `bias = 0` (trạng thái mới, chưa biết bias), reset ma trận hiệp phương sai $P$ về giá trị khởi đầu cao ($P[0][0] = P[1][1] = 1.0$, các phần tử chéo bằng 0) và **trả về ngay** giá trị góc mới. Đây là điểm mấu chốt giúp Kalman hội tụ nhanh về tư thế mới, không bị "trễ" khi xảy ra cú ngã.

3.  **Chuẩn hóa góc đầu ra về [-180°, 180°]**: Sau khi cập nhật, góc ước lượng đầu ra cũng được wrap-around về khoảng $[-180°, 180°]$ trước khi trả về, đảm bảo giá trị Roll/Pitch luôn nằm trong dải hợp lệ.

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

> **Lưu ý khởi tạo**: `kalman_1d_init` đặt giá trị ước lượng ban đầu `x = initial_value` và hiệp phương sai sai số `P = 1.0` (uncertainty cao có chủ đích), giúp bộ lọc bám nhanh về tín hiệu thực trong vài chu kỳ đầu. Mô hình dự báo là *constant model* (`x_pred = x`, `P_pred = P + Q`), do đó đây là bộ lọc 1 trạng thái thuần làm phẳng tín hiệu, không có xử lý góc hay bias như `kalman_t`.
