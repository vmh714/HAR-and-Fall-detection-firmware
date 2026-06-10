#include "kalman_filter.h"

void kalman_init(kalman_t *kalman)
{
    // Thông số tối ưu cho Fall Detection:
    // - Q_angle cao hơn: bộ lọc phản ứng nhạy với thay đổi góc đột ngột
    // - Q_bias thấp hơn: giữ bias ổn định (gyro đã calibrate tốt)
    // - R_measure vừa phiên: tin accel lúc đứng yên, bỏ qua lúc va chạm
    kalman->Q_angle   = 0.005f;   // Tăng 5x: phản ứng nhanh hơn khi ngã
    kalman->Q_bias    = 0.001f;   // Giảm 3x: bias ấn định (calibrate rồi)
    kalman->R_measure = 0.05f;    // Tăng nhẹ: giảm tin tưởng accel khi va chạm

    kalman->angle = 0.0f;
    kalman->bias  = 0.0f;

    kalman->P[0][0] = 0.0f;
    kalman->P[0][1] = 0.0f;
    kalman->P[1][0] = 0.0f;
    kalman->P[1][1] = 0.0f;
}

float kalman_get_angle(kalman_t *kalman, float new_angle, float new_rate, float dt)
{
    // Bước 1: Dự đoán trạng thái (Predict)
    float rate = new_rate - kalman->bias;
    kalman->angle += dt * rate;

    // Bước 2: Cập nhật error covariance
    kalman->P[0][0] += dt * (dt * kalman->P[1][1] - kalman->P[0][1] - kalman->P[1][0] + kalman->Q_angle);
    kalman->P[0][1] -= dt * kalman->P[1][1];
    kalman->P[1][0] -= dt * kalman->P[1][1];
    kalman->P[1][1] += kalman->Q_bias * dt;

    // Bước 3: Kalman Gain
    float S  = kalman->P[0][0] + kalman->R_measure;
    float K[2];
    K[0] = kalman->P[0][0] / S;
    K[1] = kalman->P[1][0] / S;

    // Bước 4: Sai số giữa đo lường và dự đoán
    float y = new_angle - kalman->angle;

    // --- XỬ LÝ ANGLE WRAP-AROUND [-180, 180] ---
    if (y >  180.0f) y -= 360.0f;
    if (y < -180.0f) y += 360.0f;

    // --- XỬ LÝ NHẢY GÓC ĐỘT NGỘT (Fall Detection Jump) ---
    // Ngưỡng 90°: khi nguời ngã từ đứng sang nằm (Roll 90° → 0°) thì đây là jump hợp lệ cần theo.
    // Điều chỉnh: khi jump lớn trong một bước, reset thẳng đến giá trị mới
    // cùng với reset P matrix để Kalman hội tụ nhanh về trạng thái mới.
    if (y > 90.0f || y < -90.0f) {
        kalman->angle    = new_angle;
        kalman->bias     = 0.0f;         // Reset bias: trạng thái mới, chưa biết bias
        // Reset P về giá trị khởi đầu cao: lấy mẫu mới nhanh hơn
        kalman->P[0][0]  = 1.0f;
        kalman->P[0][1]  = 0.0f;
        kalman->P[1][0]  = 0.0f;
        kalman->P[1][1]  = 1.0f;
        return kalman->angle;
    }

    // Bước 5: Cập nhật trạng thái
    kalman->angle += K[0] * y;
    kalman->bias  += K[1] * y;

    // Bước 6: Cập nhật lại ma trận hiệp phương sai
    float P00_temp = kalman->P[0][0];
    float P01_temp = kalman->P[0][1];

    kalman->P[0][0] -= K[0] * P00_temp;
    kalman->P[0][1] -= K[0] * P01_temp;
    kalman->P[1][0] -= K[1] * P00_temp;
    kalman->P[1][1] -= K[1] * P01_temp;

    // --- CHUẨN HÓA GÓC ĐẦU RA [-180, 180] ---
    if (kalman->angle >  180.0f) kalman->angle -= 360.0f;
    if (kalman->angle < -180.0f) kalman->angle += 360.0f;

    return kalman->angle;
}

// ===== Bộ lọc Kalman 1D (Scalar) =====

void kalman_1d_init(kalman_1d_t *kf, float Q, float R, float initial_value)
{
    kf->Q = Q;
    kf->R = R;
    kf->x = initial_value;
    kf->P = 1.0f; // Bắt đầu với uncertainty cao để hội tụ nhanh
}

float kalman_1d_update(kalman_1d_t *kf, float measurement)
{
    // Predict (constant model: x_pred = x, P_pred = P + Q)
    kf->P += kf->Q;

    // Update
    float K = kf->P / (kf->P + kf->R);  // Kalman Gain
    kf->x += K * (measurement - kf->x);  // State update
    kf->P *= (1.0f - K);                 // Covariance update

    return kf->x;
}