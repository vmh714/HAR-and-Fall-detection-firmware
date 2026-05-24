#ifndef IMU_SERVICE_H
#define IMU_SERVICE_H

#include "driver/gpio.h"
#include "mpu6050.h"

// Kích thước của Sliding Window (100 mẫu = 1 giây ở 100Hz)
#define IMU_WINDOW_SIZE 100
#define IMU_BATCH_SIZE 50

typedef struct __attribute__((packed)) {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
} imu_stream_data_t;

typedef struct {
    imu_stream_data_t data[IMU_BATCH_SIZE];
    uint16_t count;
} imu_batch_data_t;

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
void imu_service_get_latest_angles(float *roll, float *pitch);

typedef esp_err_t (*imu_batch_callback_t)(const void *batch_data);
void imu_service_register_batch_callback(imu_batch_callback_t cb);

#endif // IMU_SERVICE_H