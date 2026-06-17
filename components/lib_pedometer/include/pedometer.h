#ifndef PEDOMETER_H
#define PEDOMETER_H

#include <stdbool.h>
#include <stdint.h>

/// Bộ đếm bước (pedometer) THUẦN THUẬT TOÁN, platform-independent:
/// magnitude gia tốc → band-pass 0.5–3.5Hz (dải nhịp gait) → peak-detect ngưỡng động
/// + thời gian trơ. KHÔNG phụ thuộc phần cứng hay HAR — việc gate (chỉ đếm khi đi/chạy)
/// và gán nhãn walk/run do tầng gọi (svc_imu) quyết định.
typedef struct {
    // Hệ số lọc (tính từ fs trong pedometer_init)
    float hp_alpha;   ///< Hệ số high-pass (loại trọng lực/DC + trôi chậm)
    float lp_alpha;   ///< Hệ số low-pass (loại rung/nhiễu tần cao)
    // Trạng thái bộ lọc IIR
    float hp_prev_in;
    float hp_prev_out;
    float lp_prev_out;
    // Bao biên độ (envelope) cho ngưỡng động
    float env_max;
    float env_min;
    float decay;      ///< Tốc độ suy giảm envelope mỗi mẫu
    // Phát hiện đỉnh
    float prev_bp;               ///< Giá trị band-pass mẫu trước (bắt cạnh xuống qua ngưỡng)
    uint32_t samples_since_step; ///< Số mẫu kể từ bước gần nhất (cho thời gian trơ)
    // Tham số tinh chỉnh (caller có thể chỉnh sau init)
    uint32_t refractory_samples; ///< Số mẫu trơ tối thiểu giữa 2 bước
    float min_p2p;               ///< Biên độ đỉnh-đỉnh tối thiểu (g) để tính là bước thật
} pedometer_t;

/**
 * @brief Khởi tạo bộ đếm bước.
 * @param ped Con trỏ tới đối tượng pedometer.
 * @param fs_hz Tần số lấy mẫu (Hz), ví dụ 100.
 */
void pedometer_init(pedometer_t *ped, float fs_hz);

/**
 * @brief Xử lý một mẫu gia tốc 3 trục; trả về true nếu mẫu này tạo thành một bước.
 * @param ped Con trỏ pedometer.
 * @param ax Gia tốc trục X (đơn vị g).
 * @param ay Gia tốc trục Y (đơn vị g).
 * @param az Gia tốc trục Z (đơn vị g).
 * @return true nếu phát hiện một bước tại mẫu này.
 *
 * @note Nên gọi MỖI mẫu (kể cả khi đứng yên) để bộ lọc liên tục, tránh transient khi
 *       có khoảng trống. Nên cấp accel THÔ (chưa qua bộ lọc làm mượt) để đỉnh bước rõ.
 */
bool pedometer_process(pedometer_t *ped, float ax, float ay, float az);

#endif  // PEDOMETER_H
