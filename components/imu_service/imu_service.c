#include "imu_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kalman_filter.h"
#include <math.h>
#include <string.h>

static const char *TAG = "IMU_SERVICE";
static TaskHandle_t imu_task_handle = NULL;
static kalman_t kal_roll, kal_pitch;
static float last_roll = 0, last_pitch = 0;
static imu_window_t imu_win;

#define RAD_TO_DEG 57.2957795131f

// ISR Handler - Chỉ gác cổng, không xử lý nặng
static void IRAM_ATTR imu_isr_handler(void *arg)
{
    static uint32_t count_pkg = 0;
    if (++count_pkg >= IMU_BATCH_SIZE) {
        count_pkg = 0;
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(imu_task_handle, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

// Task xử lý dữ liệu chính
static void imu_processing_task(void *pvParameters)
{
    mpu6050_data_raw_t raw_data[IMU_BATCH_SIZE];
    uint16_t count;
    
    // Lấy sample rate thực tế từ driver (tránh hằng số cứng)
    uint16_t sample_rate = mpu6050_get_sample_rate();
    if (sample_rate == 0) sample_rate = 100; // Phòng hờ lỗi
    float dt = 1.0f / (float)sample_rate;

    while (1) {
        // Đợi thông báo từ ngắt (mỗi 10ms - Data Ready)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        count = IMU_BATCH_SIZE;
        // Đọc dữ liệu từ FIFO (Burst Read)
        if (mpu6050_read_fifo(raw_data, &count) == ESP_OK && count > 0) {
            
            for (int i = 0; i < count; i++) {
                // Chuyển đổi Raw -> Float (Sử dụng SSF mặc định/hiện tại)
                mpu6050_data_t processed_data;
                // Tự động dùng đúng SSF từ cấu hình hiện tại của MPU
                mpu6050_raw_to_float(&raw_data[i], &processed_data);

                // Chuyển đổi trục (Mapping) sang hệ tọa độ Body (Forward-Left-Up FLU)
                // Yêu cầu: Hướng tiến tới (Forward = X_body) là trục Z của mạch.
                // Chiều thẳng đứng lên trời (Up = Z_body) đang là trục Y của cảm biến (log lúc nghỉ ay ~ 1g).
                // Do đó để có (Roll = 0, Pitch = 0) khi dựng đứng trên Y:
                float ax_body = processed_data.az;
                float ay_body = processed_data.ax;
                float az_body = processed_data.ay; // Positivie để về 0 độ ở rest state

                float gx_body = processed_data.gz;
                float gy_body = processed_data.gx;
                
                // Tính góc từ gia tốc kế
                float accel_roll = atan2(ay_body, az_body) * RAD_TO_DEG;
                float accel_pitch = atan2(-ax_body, sqrt(ay_body * ay_body + az_body * az_body)) * RAD_TO_DEG;
                
                if (fabsf(accel_pitch) > 75.0f) {
    kal_roll.R_measure = 2.0f;   // Tăng mạnh: bỏ qua noise Roll accel
} else {
    kal_roll.R_measure = 0.05f;  // Bình thường
}
                // Lọc Kalman đã được đồng bộ dấu 
                last_roll = kalman_get_angle(&kal_roll, accel_roll, gx_body, dt);
                last_pitch = kalman_get_angle(&kal_pitch, accel_pitch, gy_body, dt);

                // Cập nhật Sliding Window
                imu_win.roll[imu_win.head] = last_roll;
                imu_win.pitch[imu_win.head] = last_pitch;
                imu_win.head = (imu_win.head + 1) % IMU_WINDOW_SIZE;
            }

            // In log mỗi khi xử lý xong một batch (0.5s)
            //ESP_LOGI(TAG, "Batch processed: %d samples. Current Roll: %.2f, Pitch: %.2f", ount, last_roll, last_pitch);
        }
    }
}

esp_err_t imu_service_init(gpio_num_t int_pin)
{
    // 1. Khởi tạo bộ lọc Kalman
    kalman_init(&kal_roll);
    kalman_init(&kal_pitch);
    memset(&imu_win, 0, sizeof(imu_window_t));

    // --- HOT START: Lấy góc ban đầu để tránh bộ lọc bị leo từ 0 ---
    mpu6050_data_t init_data = mpu6050_read();
    
    // Đủ cẩn thận để áp dụng chung Mapping từ đầu (Hệ FLU)
    float ax_body = init_data.az;
    float ay_body = init_data.ax;
    float az_body = init_data.ay;

    float init_roll = atan2(ay_body, az_body) * RAD_TO_DEG;
    float init_pitch = atan2(-ax_body, sqrt(ay_body * ay_body + az_body * az_body)) * RAD_TO_DEG;
    
    kal_roll.angle = init_roll;
    kal_pitch.angle = init_pitch;
    last_roll = init_roll;
    last_pitch = init_pitch;

    ESP_LOGI(TAG, "Hot start completed. Initial Roll: %.2f, Pitch: %.2f", init_roll, init_pitch);

    // --- SỬA LỖI FIFO TRÀN (Làm lệch byte dữ liệu) ---
    // Trong quá trình calib 2 giây, FIFO đã bị tràn và byte bị lệch. 
    // Ta phải reset FIFO cho sạch rác trước khi bắt đầu Task!
    mpu6050_reset_fifo();

    // 2. Tạo Task xử lý (độ ưu tiên cao)
    xTaskCreate(imu_processing_task, "imu_task", 4096, NULL, 10, &imu_task_handle);

    // 3. Cấu hình GPIO cho ngắt
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << int_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);

    // 4. Cài đặt ISR
    gpio_install_isr_service(0);
    gpio_isr_handler_add(int_pin, imu_isr_handler, NULL);

    ESP_LOGI(TAG, "IMU Service initialized on GPIO %d", int_pin);
    return ESP_OK;
}

void imu_service_get_latest_angles(float *roll, float *pitch)
{
    *roll = last_roll;
    *pitch = last_pitch;
}
