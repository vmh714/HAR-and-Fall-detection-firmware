#ifndef _KALMAN_FILTER_H
#define _KALMAN_FILTER_H

typedef struct
{
    // Các tham số nhiễu (Tuning parameters)
    float Q_angle;   // Phương sai nhiễu của gia tốc kế
    float Q_bias;    // Phương sai nhiễu trôi của gyro
    float R_measure; // Nhiễu đo lường (Càng lớn, hệ thống càng tin tưởng Gyro hơn Accel)

    // Trạng thái (State)
    float angle; // Góc đã được lọc
    float bias;  // Sai số trôi (drift) của Gyro tính toán được

    // Ma trận hiệp phương sai sai số 2x2 (Error Covariance Matrix)
    float P[2][2];
} kalman_t;

// Khởi tạo thông số mặc định cho màng lọc
void kalman_init(kalman_t *kalman);

// Hàm cập nhật bộ lọc (gọi liên tục trong vòng lặp)
float kalman_get_angle(kalman_t *kalman, float new_angle, float new_rate, float dt);

// ===== Bộ lọc Kalman 1D (Scalar) cho signal smoothing =====
// Dùng để lọc nhiễu từng trục IMU riêng lẻ (ax, ay, az, gx, gy, gz)
// Khác với kalman_t (2-state, có bias + angle wrap): đây là 1-state, không có
// xử lý góc, phù hợp cho tín hiệu gia tốc/vận tốc góc thô.
typedef struct {
    float x;  // Giá trị ước lượng (state)
    float P;  // Hiệp phương sai sai số (error covariance)
    float Q;  // Nhiễu quá trình (process noise) — càng lớn càng bám sát tín hiệu
    float R;  // Nhiễu đo lường (measurement noise) — càng lớn càng mượt
} kalman_1d_t;

void kalman_1d_init(kalman_1d_t *kf, float Q, float R, float initial_value);
float kalman_1d_update(kalman_1d_t *kf, float measurement);

#endif