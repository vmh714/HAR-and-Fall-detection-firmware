#ifndef TFLITE_WRAPPER_H
#define TFLITE_WRAPPER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo mô hình TFLite Micro và cấp phát bộ nhớ Arena
// Trả về 0 nếu thành công, -1 nếu thất bại
int tflite_init(void);

// Thực thi mô hình với dữ liệu giả định và đo thời gian
void tflite_run_inference(void);

#include <stdbool.h>
#include <stdint.h>

// Các nhãn phân loại từ mô hình AI (phải khớp với class list khi train)
typedef enum {
    AI_CLASS_WALK = 0,
    AI_CLASS_RUN = 1,
    AI_CLASS_IDLE = 2,
    AI_CLASS_TRANSITION = 3,
    AI_CLASS_FALL = 4,
    AI_CLASS_UNKNOWN = 5
} ai_posture_class_t;

// Struct chứa kết quả trả về từ mô hình AI
typedef struct {
  ai_posture_class_t predicted_class;
  float max_prob;            // Xác suất của predicted_class (0.0 - 1.0)
  float fall_prob;           // Xác suất của class Fall
  int64_t inference_time_us; // Thời gian thực thi (microseconds)
  bool is_valid;             // true nếu inference thành công
} ai_inference_result_t;

// Hàm phụ trợ cho dữ liệu thực tế
int get_input_bytes(void);
ai_inference_result_t tflite_run_inference_with_data(float *data,
                                                     size_t num_bytes);

#ifdef __cplusplus
}
#endif

#endif // TFLITE_WRAPPER_H
