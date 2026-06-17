#ifndef TFLITE_WRAPPER_H
#define TFLITE_WRAPPER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo mô hình TFLite Micro và cấp phát bộ nhớ Tensor Arena.
 * @return 0 nếu thành công, -1 nếu thất bại.
 */
int tflite_init(void);

/**
 * @brief Thực thi mô hình với dữ liệu giả định (toàn 0) và đo thời gian inference.
 */
void tflite_run_inference(void);

#include <stdbool.h>
#include <stdint.h>

/// Các nhãn phân loại từ mô hình AI — thứ tự PHẢI khớp với class list khi train.
typedef enum {
    AI_CLASS_WALK = 0,
    AI_CLASS_RUN = 1,
    AI_CLASS_IDLE = 2,
    AI_CLASS_TRANSITION = 3,
    AI_CLASS_FALL = 4,
    AI_CLASS_UNKNOWN = 5
} ai_posture_class_t;

/// Struct chứa kết quả trả về từ mô hình AI.
typedef struct {
  ai_posture_class_t predicted_class;
  float max_prob;            // Xác suất của predicted_class (0.0 - 1.0)
  float fall_prob;           // Xác suất của class Fall
  int64_t inference_time_us; // Thời gian thực thi (microseconds)
  bool is_valid;             // true nếu inference thành công
} ai_inference_result_t;

/**
 * @brief Lấy kích thước (byte) của tensor đầu vào tính theo định dạng FLOAT32.
 * @return Số byte cần cấp cho buffer đầu vào, hoặc 0 nếu chưa khởi tạo.
 */
int get_input_bytes(void);

/**
 * @brief Chạy inference với dữ liệu thực tế từ IMU và trả về kết quả phân loại.
 * @param data Con trỏ tới mảng float đầu vào (đã chuẩn hóa về [-1.0, 1.0]).
 * @param num_bytes Kích thước mảng đầu vào tính bằng byte.
 * @return Cấu trúc kết quả; trường is_valid = false nếu inference thất bại.
 */
ai_inference_result_t tflite_run_inference_with_data(float *data,
                                                     size_t num_bytes);

#ifdef __cplusplus
}
#endif

#endif // TFLITE_WRAPPER_H
