#ifndef IMU_SERVICE_H
#define IMU_SERVICE_H

#include "driver/gpio.h"
#include "mpu6050.h"

/// Sliding Window 200 mẫu = 2 giây ở 100Hz — kích thước input cố định của model TinyML
/// (không đổi mà không retrain). Trượt 50 mẫu (0.5s) mỗi lần xử lý một batch.
#define IMU_WINDOW_SIZE 200
#define IMU_BATCH_SIZE 50

typedef struct __attribute__((packed)) {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
} imu_stream_data_t;

typedef struct {
    imu_stream_data_t data[IMU_BATCH_SIZE];
    uint16_t count;
} imu_batch_data_t;

/// Sliding window chứa 6 trục IMU đã lọc Kalman và chuẩn hóa về (-1, 1):
/// định dạng input trực tiếp cho model TinyML đã quantize INT8.
typedef struct {
    float ax[IMU_WINDOW_SIZE];
    float ay[IMU_WINDOW_SIZE];
    float az[IMU_WINDOW_SIZE];
    float gx[IMU_WINDOW_SIZE];
    float gy[IMU_WINDOW_SIZE];
    float gz[IMU_WINDOW_SIZE];
    uint16_t head;
} imu_window_t;

/**
 * @brief Khởi tạo IMU Service: cấu hình MPU6050, bộ lọc Kalman, PCNT đếm xung INT
 *        và tạo task xử lý dữ liệu.
 * @param int_pin Chân GPIO được nối với chân INT của MPU6050.
 * @return ESP_OK nếu khởi tạo thành công.
 */
esp_err_t imu_service_init(gpio_num_t int_pin);

/**
 * @brief Lấy giá trị góc pitch mới nhất (đã lọc Kalman) để xác định tư thế.
 * @param pitch Con trỏ nhận giá trị pitch (đơn vị độ).
 */
void imu_service_get_latest_roll(float *roll);

typedef esp_err_t (*imu_batch_callback_t)(const void *batch_data);

/**
 * @brief Đăng ký callback nhận một lô (batch) dữ liệu IMU đã tiền xử lý khi ở STATE_STREAMING.
 * @param cb Hàm callback được gọi mỗi khi gom đủ IMU_BATCH_SIZE mẫu.
 */
void imu_service_register_batch_callback(imu_batch_callback_t cb);

/**
 * @brief Lấy số bước chân tích lũy (đi bộ/chạy) đếm được trên thiết bị (pedometer).
 * @param walk Con trỏ nhận số bước đi bộ (có thể NULL).
 * @param run  Con trỏ nhận số bước chạy (có thể NULL).
 */
void imu_service_get_steps(uint32_t *walk, uint32_t *run);

typedef struct {
    int64_t ts_us;
    float peak_g;
    bool had_freefall;
} imu_impact_info_t;

/**
 * @brief Lấy thông tin cú va chạm (impact) gần nhất được ghi nhận.
 * @param out Con trỏ cấu trúc nhận dữ liệu va chạm.
 */
void imu_service_get_last_impact(imu_impact_info_t *out);

#endif // IMU_SERVICE_H