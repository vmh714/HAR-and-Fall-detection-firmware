#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "mpu6050.h"
#include "imu_service.h"
#include <math.h>

static const char *TAG = "MAIN_APP";

#define IMU_INT_GPIO GPIO_NUM_11
#define I2C_PORT I2C_NUM_0
#define I2C_SCL GPIO_NUM_9
    #define I2C_SDA GPIO_NUM_10
#define RAD_TO_DEG 57.2957795131f

void app_main(void)
{
    // 1. Khởi tạo Bus I2C
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    // 2. Khởi tạo MPU6050
    mpu6050_init(bus_handle);

    // 3. Cấu hình chi tiết (FIFO + Interrupt)
    mpu6050_config_t my_cfg = {
        .accel_fs = ACCEL_FS_8G,
        .gyro_fs = GYRO_FS_500DPS,
        .dlpf_cfg = DLPF_CFG_21HZ,
        .sample_rate_hz = 100, // 100Hz = 10ms/mẫu
        
        .pwr_cfg = {
            .temp_disable = true, // Vẫn bật cảm biến nhiệt nếu cần
        },
        
        .int_cfg = {
            .data_ready_en = true,    // Bật ngắt mỗi khi có dữ liệu mới
            .active_low = true,       // Chân INT mức thấp khi có ngắt
            .latch_en = false,        // Ngắt tự xóa sau 50us
        },

        .fifo_cfg = {
            .fifo_enable = true,      // Bật module FIFO
            .accel_fifo_en = true,    // Đẩy Accel vào FIFO
            .gyro_fifo_en = true,     // Đẩy Gyro vào FIFO
            .temp_fifo_en = false,    // Không cần Temp trong FIFO để tiết kiệm
        }
    };
    mpu6050_config(&my_cfg);

    // 4. Calib Gyro (Quan trọng để khử trôi)
    mpu6050_calibrate_gyro();

    // 5. Chạy IMU Service (Tự động tạo Task và ISR)
    imu_service_init(IMU_INT_GPIO);

    ESP_LOGI(TAG, "System optimized architecture started!");
    mpu6050_data_t data;
    float roll_filtered, pitch_filtered;
    float roll, pitch;
    while (1)
    {
        // Lấy dữ liệu góc mới nhất từ Service (đã qua lọc Kalman)
        imu_service_get_latest_angles(&roll_filtered, &pitch_filtered);
        data = mpu6050_read();

        // Axis mapping (nhất quán với imu_service.c):
        //   X_body (sang ngang)  = Az (old Z)
        //   Y_body (lên trời)    = Ax (old X)  ← trùng hướng trọng lực khi đeo
        //   Z_body (hướng tiến)  = Ay (old Y)
        float ax_b = data.az;  // X body
        float ay_b = data.ax;  // Y body (UP)
        float az_b = data.ay;  // Z body (Forward)
        roll  = atan2f(ay_b, az_b) * RAD_TO_DEG;
        pitch = atan2f(-ax_b, sqrtf(ay_b * ay_b + az_b * az_b)) * RAD_TO_DEG;

        ESP_LOGI("RAW", "Ax: %.2f | Ay: %.2f | Az: %.2f | Temp: %.2f | Gx: %.2f | Gy: %.2f | Gz: %.2f", data.ax, data.ay, data.az, data.temp, data.gx, data.gy, data.gz);
        ESP_LOGI("RAW", "[BEFORE FILTER]Accel Roll: %.2f, Pitch: %.2f", roll, pitch);
        ESP_LOGI("RESULT", "[Filtered] Roll: %.2f | Pitch: %.2f", roll_filtered, pitch_filtered);
        
        vTaskDelay(pdMS_TO_TICKS(1000)); // Log ra màn hình mỗi 1 giây
    }
}
