#ifndef SVC_AI_H
#define SVC_AI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "imu_service.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Khởi tạo Service AI (Inference Task)
     *
     * Khởi tạo Task để chạy mô hình AI liên tục khi có dữ liệu đầu vào.
     * Hàm này sẽ gọi tflite_init() bên trong.
     *
     * @return esp_err_t ESP_OK nếu khởi tạo thành công
     */
    esp_err_t svc_ai_init(void);

    /**
     * @brief Nhận dữ liệu window từ svc_imu để thực thi suy luận (inference)
     *
     * Hàm này sẽ được gọi qua Callback từ svc_imu khi có đủ 1 sliding window.
     * Nó sẽ gửi dữ liệu vào Queue để Task AI xử lý không đồng bộ (không block
     * IMU ISR).
     *
     * @param window Con trỏ tới struct imu_window_t chứa RingBuffer dữ liệu IMU
     */
    void svc_ai_process_window(const imu_window_t* window);

    /**
     * @brief Lấy kết quả dự đoán (chuỗi text) gần nhất của AI
     * @return const char* (ví dụ "Walk", "Run", "Idle", "Fall")
     */
    const char* svc_ai_get_latest_prediction(void);

    /**
     * @brief Lấy độ tự tin (confidence) của kết quả dự đoán gần nhất
     * @return float (từ 0.0 đến 1.0)
     */
    float svc_ai_get_latest_confidence(void);

#ifdef __cplusplus
}
#endif

#endif  // SVC_AI_H
