#include "imu_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kalman_filter.h"
#include "driver/pulse_cnt.h"
#include <math.h>
#include <string.h>
#include "sys_manager.h"
#include "esp_event.h"
#include "svc_ai.h"

static const char *TAG = "IMU_SERVICE";
static TaskHandle_t imu_task_handle = NULL;
static pcnt_unit_handle_t pcnt_unit = NULL;
static kalman_1d_t kf_ax, kf_ay, kf_az, kf_gx, kf_gy, kf_gz;
static kalman_t kal_pitch; // 2-state Kalman riêng cho xác định tư thế (nằm/đứng/ngồi)
static float last_pitch = 0;
static imu_window_t imu_win;
static imu_batch_data_t s_batch_data;
static imu_batch_callback_t s_batch_callback = NULL;

#define RAD_TO_DEG 57.2957795131f

// Dải đo thực tế, tính từ accel_ssf/gyro_ssf có sẵn trong mpu6050.h
static float s_accel_fs_range;
static float s_gyro_fs_range;

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
        // Đợi thông báo từ ngắt (mỗi ~500ms - đếm đủ 50 mẫu) với timeout 1 giây để chống kẹt
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (notified == 0) {
            ESP_LOGW(TAG, "Timeout waiting for PCNT interrupt. Possible MPU6050 FIFO/INT hang. Recovering...");
            mpu6050_reset_fifo();
            pcnt_unit_clear_count(pcnt_unit);
            continue;
        }

        count = IMU_BATCH_SIZE;
        // Đọc dữ liệu từ FIFO (Burst Read)
        if (mpu6050_read_fifo(raw_data, &count) == ESP_OK && count > 0) {
            bool is_streaming = (sys_manager_get_state() == STATE_STREAMING);
            if (!is_streaming) {
                s_batch_data.count = 0;
            }
            
            for (int i = 0; i < count; i++) {
                mpu6050_data_t processed_data;
                mpu6050_raw_to_float(&raw_data[i], &processed_data);

                // Chuyển đổi trục: Sensor frame → Body frame (Forward-Left-Up)
                float ax_body = -processed_data.ax;
                float ay_body = -processed_data.ay;
                float az_body = processed_data.az;
                float gx_body = -processed_data.gx;
                float gy_body = -processed_data.gy;
                float gz_body = processed_data.gz;

                // Pitch chỉ dùng để xác định tư thế (nằm/đứng/ngồi)
                float accel_pitch = atan2(-ax_body, sqrt(ay_body * ay_body + az_body * az_body)) * RAD_TO_DEG;
                last_pitch = kalman_get_angle(&kal_pitch, accel_pitch, gy_body, dt);

                // Lọc Kalman 1D từng trục IMU rồi chuẩn hóa về (-1, 1)
                // cho TinyML model đã quantize INT8
                float filt_ax = kalman_1d_update(&kf_ax, ax_body);
                float filt_ay = kalman_1d_update(&kf_ay, ay_body);
                float filt_az = kalman_1d_update(&kf_az, az_body);
                float filt_gx = kalman_1d_update(&kf_gx, gx_body);
                float filt_gy = kalman_1d_update(&kf_gy, gy_body);
                float filt_gz = kalman_1d_update(&kf_gz, gz_body);

                // Chuẩn hóa về thang (-1, 1) theo full-scale range (tự tính từ config)
                imu_win.ax[imu_win.head] = filt_ax / s_accel_fs_range;
                imu_win.ay[imu_win.head] = filt_ay / s_accel_fs_range;
                imu_win.az[imu_win.head] = filt_az / s_accel_fs_range;
                imu_win.gx[imu_win.head] = filt_gx / s_gyro_fs_range;
                imu_win.gy[imu_win.head] = filt_gy / s_gyro_fs_range;
                imu_win.gz[imu_win.head] = filt_gz / s_gyro_fs_range;
                imu_win.head = (imu_win.head + 1) % IMU_WINDOW_SIZE;

                // Nếu đang stream, copy data vào batch
                // LƯU Ý QUAN TRỌNG: Phải stream data đã đổi hệ trục (Body frame) 
                // và đã qua bộ lọc Kalman 1D để đồng nhất với data inference của TinyML.
                // Scale lại về int16_t ([-32767, 32767]) tương ứng với mốc [-1.0, 1.0] của imu_win.
                if (is_streaming && s_batch_data.count < IMU_BATCH_SIZE) {
                    s_batch_data.data[s_batch_data.count].ax = (int16_t)((filt_ax / s_accel_fs_range) * 32767.0f);
                    s_batch_data.data[s_batch_data.count].ay = (int16_t)((filt_ay / s_accel_fs_range) * 32767.0f);
                    s_batch_data.data[s_batch_data.count].az = (int16_t)((filt_az / s_accel_fs_range) * 32767.0f);
                    s_batch_data.data[s_batch_data.count].gx = (int16_t)((filt_gx / s_gyro_fs_range) * 32767.0f);
                    s_batch_data.data[s_batch_data.count].gy = (int16_t)((filt_gy / s_gyro_fs_range) * 32767.0f);
                    s_batch_data.data[s_batch_data.count].gz = (int16_t)((filt_gz / s_gyro_fs_range) * 32767.0f);
                    s_batch_data.count++;
                }
            }

            if (is_streaming && s_batch_data.count >= IMU_BATCH_SIZE) {
                ESP_LOGI(TAG, "Batch ready (%d samples). Enqueuing via callback.", s_batch_data.count);
                if (s_batch_callback != NULL) {
                    esp_err_t err = s_batch_callback(&s_batch_data);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to enqueue IMU batch: %s", esp_err_to_name(err));
                    }
                } else {
                    ESP_LOGW(TAG, "No batch callback registered!");
                }
                s_batch_data.count = 0;
            }

            // Gửi RingBuffer cho svc_ai sau mỗi lô 50 mẫu (Slide Window 0.5s)
            // Việc này chạy song song với streaming (nếu có). Ở STATE_NORMAL, AI vẫn hoạt động.
            if (sys_manager_get_state() == STATE_NORMAL || sys_manager_get_state() == STATE_STREAMING) {
                // Ta gọi svc_ai_process_window() ở đây vì `count` đọc từ FIFO luôn bằng IMU_BATCH_SIZE (50)
                // Vậy cứ qua vòng lặp xử lý 50 mẫu, ta gửi window 1 lần (Trượt 0.5s)
                svc_ai_process_window(&imu_win);
            }

            // In log mỗi khi xử lý xong một batch (0.5s)
            //ESP_LOGI(TAG, "Batch processed: %d samples. Current Roll: %.2f, Pitch: %.2f", count, last_roll, last_pitch);
        }
    }
}

esp_err_t imu_service_init(gpio_num_t int_pin)
{
    // 1. Cấu hình MPU6050 (tập trung tại đây, không hardcode ngoài app_main)
    mpu6050_config_t imu_cfg = {
        .accel_fs = ACCEL_FS_8G,
        .gyro_fs = GYRO_FS_2000DPS,
        .dlpf_cfg = DLPF_CFG_21HZ,
        .sample_rate_hz = 100,
        .pwr_cfg = { .temp_disable = true },
        .int_cfg = {
            .data_ready_en = true,
            .active_low = true,
            .latch_en = false,
        },
        .fifo_cfg = {
            .fifo_enable = true,
            .accel_fifo_en = true,
            .gyro_fifo_en = true,
            .temp_fifo_en = false,
        }
    };
    mpu6050_config(&imu_cfg);

    // Tính dải chuẩn hóa trực tiếp từ SSF đã có sẵn trong mpu6050.h
    // Công thức: fs_range = 32768 / SSF (ví dụ: 32768/4096 = ±8g)
    s_accel_fs_range = 32768.0f / accel_ssf[imu_cfg.accel_fs];
    s_gyro_fs_range  = 32768.0f / gyro_ssf[imu_cfg.gyro_fs];
    ESP_LOGI(TAG, "Normalization range: Accel=±%.0fg, Gyro=±%.0f°/s", s_accel_fs_range, s_gyro_fs_range);

    // Calibrate gyro (cần đứng yên ~3s)
    mpu6050_calibrate_gyro();

    // 2. Khởi tạo bộ lọc Kalman
    // - kalman_1d: lọc nhiễu 6 trục IMU (Q=0.01: bám nhanh, R=0.1: làm mượt vừa)
    // - kalman_t:  ước lượng góc pitch (2-state với bias + gyro fusion)
    kalman_1d_init(&kf_ax, 0.01f, 0.1f, 0);
    kalman_1d_init(&kf_ay, 0.01f, 0.1f, 0);
    kalman_1d_init(&kf_az, 0.01f, 0.1f, 0);
    kalman_1d_init(&kf_gx, 0.01f, 0.1f, 0);
    kalman_1d_init(&kf_gy, 0.01f, 0.1f, 0);
    kalman_1d_init(&kf_gz, 0.01f, 0.1f, 0);
    kalman_init(&kal_pitch);
    memset(&imu_win, 0, sizeof(imu_window_t));

    // --- HOT START: Seed với giá trị thực để tránh ramp-up từ 0 ---
    mpu6050_data_t init_data = mpu6050_read();
    float ax_body = -init_data.ax;
    float ay_body = -init_data.ay;
    float az_body = init_data.az;

    float init_pitch = atan2(-ax_body, sqrt(ay_body * ay_body + az_body * az_body)) * RAD_TO_DEG;
    kal_pitch.angle = init_pitch;
    last_pitch = init_pitch;

    // Seed 6-axis Kalman 1D với giá trị đọc ban đầu
    kf_ax.x = ax_body;  kf_ay.x = ay_body;  kf_az.x = az_body;
    kf_gx.x = -init_data.gx;  kf_gy.x = -init_data.gy;  kf_gz.x = init_data.gz;

    ESP_LOGI(TAG, "Hot start completed. Initial Pitch: %.2f", init_pitch);

    // Reset FIFO: sau calibrate ~3s, FIFO đã tràn và byte bị lệch
    mpu6050_reset_fifo();

    // 3. Tạo Task xử lý (độ ưu tiên cao)
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

void imu_service_get_latest_pitch(float *pitch)
{
    *pitch = last_pitch;
}

void imu_service_register_batch_callback(imu_batch_callback_t cb)
{
    s_batch_callback = cb;
}
