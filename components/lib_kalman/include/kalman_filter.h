#ifndef _KALMAN_FILTER_H
#define _KALMAN_FILTER_H

/**
 * @brief Trạng thái bộ lọc Kalman 2-state (góc + bias gyro) dùng cho ước lượng góc nghiêng.
 *
 * Mô hình 2 biến trạng thái: góc đã lọc và sai số trôi (bias) của con quay hồi chuyển.
 */
typedef struct
{
    // Các tham số nhiễu (Tuning parameters)
    /// Q_angle: phương sai nhiễu quá trình của góc — đã tune cao cho fall detection để bám nhanh thay đổi góc đột ngột.
    float Q_angle;   // Phương sai nhiễu của gia tốc kế
    /// Q_bias: phương sai nhiễu trôi của gyro — tune thấp vì gyro đã calibrate, giữ bias ổn định.
    float Q_bias;    // Phương sai nhiễu trôi của gyro
    /// R_measure: nhiễu đo lường — càng lớn càng tin Gyro hơn Accel; tune để bỏ qua accel lúc va chạm.
    float R_measure; // Nhiễu đo lường (Càng lớn, hệ thống càng tin tưởng Gyro hơn Accel)

    // Trạng thái (State)
    float angle; // Góc đã được lọc
    float bias;  // Sai số trôi (drift) của Gyro tính toán được

    // Ma trận hiệp phương sai sai số 2x2 (Error Covariance Matrix)
    float P[2][2];
} kalman_t;

/**
 * @brief Khởi tạo thông số mặc định cho bộ lọc Kalman 2-state.
 * @param kalman Con trỏ tới cấu trúc bộ lọc cần khởi tạo.
 */
void kalman_init(kalman_t *kalman);

/**
 * @brief Cập nhật bộ lọc và trả về góc đã lọc (gọi liên tục trong vòng lặp).
 * @param kalman Con trỏ tới cấu trúc bộ lọc.
 * @param new_angle Góc đo từ gia tốc kế (độ).
 * @param new_rate Vận tốc góc đo từ gyro (độ/giây).
 * @param dt Khoảng thời gian giữa hai lần cập nhật (giây).
 * @return Góc đã lọc, đã chuẩn hóa về [-180, 180].
 */
float kalman_get_angle(kalman_t *kalman, float new_angle, float new_rate, float dt);

// ===== Bộ lọc Kalman 1D (Scalar) cho signal smoothing =====
/// Dùng để lọc nhiễu từng trục IMU riêng lẻ (ax, ay, az, gx, gy, gz).
/// Khác với kalman_t (2-state, có bias + angle wrap): đây là 1-state, không có
/// xử lý góc, phù hợp cho tín hiệu gia tốc/vận tốc góc thô.
/**
 * @brief Trạng thái bộ lọc Kalman 1D (scalar) dùng làm bộ lọc mượt tín hiệu.
 */
typedef struct {
    float x;  // Giá trị ước lượng (state)
    float P;  // Hiệp phương sai sai số (error covariance)
    /// Q: nhiễu quá trình — càng lớn càng bám sát tín hiệu (ít trễ, nhiều nhiễu).
    float Q;  // Nhiễu quá trình (process noise) — càng lớn càng bám sát tín hiệu
    /// R: nhiễu đo lường — càng lớn càng mượt (nhiều trễ, ít nhiễu).
    float R;  // Nhiễu đo lường (measurement noise) — càng lớn càng mượt
} kalman_1d_t;

/**
 * @brief Khởi tạo bộ lọc Kalman 1D.
 * @param kf Con trỏ tới cấu trúc bộ lọc.
 * @param Q Nhiễu quá trình.
 * @param R Nhiễu đo lường.
 * @param initial_value Giá trị ước lượng khởi đầu.
 */
void kalman_1d_init(kalman_1d_t *kf, float Q, float R, float initial_value);

/**
 * @brief Cập nhật bộ lọc Kalman 1D với một giá trị đo mới.
 * @param kf Con trỏ tới cấu trúc bộ lọc.
 * @param measurement Giá trị đo mới.
 * @return Giá trị ước lượng đã lọc.
 */
float kalman_1d_update(kalman_1d_t *kf, float measurement);

#endif