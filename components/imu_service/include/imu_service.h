#ifndef IMU_SERVICE_H
#define IMU_SERVICE_H

#include "driver/gpio.h"
#include "mpu6050.h"

// Kích thước của Sliding Window (100 mẫu = 1 giây ở 100Hz)
#define IMU_WINDOW_SIZE 100
#define IMU_BATCH_SIZE 50

typedef struct {
    float roll[IMU_WINDOW_SIZE];
    float pitch[IMU_WINDOW_SIZE];
    uint16_t head; // Chỉ số hiện tại của bộ đệm vòng
} imu_window_t;

/**
 * @brief Khởi tạo IMU Service, bao gồm cấu hình GPIO Ngắt, tạo Task xử lý
 * 
 * @param int_pin Chân GPIO được nối với chân INT của MPU6050
 * @return esp_err_t 
 */
esp_err_t imu_service_init(gpio_num_t int_pin);

/**
 * @brief Lấy dữ liệu góc mới nhất
 * 
 * @param roll Con trỏ lưu giá trị roll
 * @param pitch Con trỏ lưu giá trị pitch
 */
void imu_service_get_latest_angles(float *roll, float *pitch);

#endif // IMU_SERVICE_H