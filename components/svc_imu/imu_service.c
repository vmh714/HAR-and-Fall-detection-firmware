#include "imu_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kalman_filter.h"
#include "driver/pulse_cnt.h"
#include <math.h>
#include <string.h>

static const char *TAG = "IMU_SERVICE";
static TaskHandle_t imu_task_handle = NULL;
static pcnt_unit_handle_t pcnt_unit = NULL;
static kalman_t kal_roll, kal_pitch;
static float last_roll = 0, last_pitch = 0;
static imu_window_t imu_win;

#define RAD_TO_DEG 57.2957795131f

// Callback từ PCNT - Chỉ gọi khi đã đếm đủ IMU_BATCH_SIZE mẫu
static bool IRAM_ATTR pcnt_on_reach(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    vTaskNotifyGiveFromISR(imu_task_handle, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
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

    // 3. Cấu hình PCNT để đếm xung từ chân INT của IMU
    // Việc này giúp CPU không phải thức dậy cho mỗi mẫu dữ liệu, 
    // mà chỉ thức dậy khi đủ một Batch (ví dụ 50 mẫu).
    pcnt_unit_config_t unit_config = {
        .high_limit = IMU_BATCH_SIZE,
        .low_limit = -1,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &pcnt_unit));

    // Lọc nhiễu (Glitch filter) - tránh đếm sai do nhiễu đường truyền
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 1000,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(pcnt_unit, &filter_config));

    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = int_pin,
        .level_gpio_num = -1, // Không dùng level control
    };
    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_config, &pcnt_chan));

    // Thiết lập hành động khi có cạnh xuống (Falling Edge - giống GPIO_INTR_NEGEDGE)
    // Cạnh lên: Không làm gì (HOLD)
    // Cạnh xuống: Tăng bộ đếm (INCREASE)
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_HOLD, PCNT_CHANNEL_EDGE_ACTION_INCREASE));

    // Đặt điểm quan sát (Watch Point) tại IMU_BATCH_SIZE để kích hoạt ngắt
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(pcnt_unit, IMU_BATCH_SIZE));

    // Đăng ký callback khi đạt đến Watch Point
    pcnt_event_callbacks_t cbs = {
        .on_reach = pcnt_on_reach,
    };
    ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(pcnt_unit, &cbs, NULL));

    // Kích hoạt và chạy bộ đếm
    ESP_ERROR_CHECK(pcnt_unit_enable(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(pcnt_unit));

    ESP_LOGI(TAG, "IMU Service initialized with PCNT on GPIO %d (Batch: %d)", int_pin, IMU_BATCH_SIZE);
    return ESP_OK;
}

void imu_service_get_latest_angles(float *roll, float *pitch)
{
    *roll = last_roll;
    *pitch = last_pitch;
}
