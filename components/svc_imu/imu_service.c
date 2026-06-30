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
#include "pedometer.h"
#include "nvs.h"
#include "esp_timer.h"

static const char *TAG = "IMU_SERVICE";
static TaskHandle_t imu_task_handle = NULL;
static pcnt_unit_handle_t pcnt_unit = NULL;
static kalman_1d_t kf_ax, kf_ay, kf_az, kf_gx, kf_gy, kf_gz;
static kalman_t kal_roll; /// 2-state Kalman riêng (góc + bias) để ước lượng tư thế: nằm/đứng/ngồi
static float last_roll = 0;
static imu_window_t imu_win;
static imu_batch_data_t s_batch_data;
static imu_batch_callback_t s_batch_callback = NULL;
static uint8_t s_i2c_consecutive_errors = 0;

#define RAD_TO_DEG 57.2957795131f

// Dải đo thực tế, tính từ accel_ssf/gyro_ssf có sẵn trong mpu6050.h
static float s_accel_fs_range;
static float s_gyro_fs_range;

#define FREEFALL_THR_G 0.6f
#define IMPACT_THR_G 2.5f

static imu_impact_info_t s_latest_impact = {0};
static bool s_temp_had_freefall = false;
static int64_t s_temp_freefall_ts = 0;
static portMUX_TYPE s_impact_mux = portMUX_INITIALIZER_UNLOCKED;


/// Pedometer: đếm bước on-device từ accel thô body-frame, gate theo HAR (Walk/Run).
static pedometer_t s_pedometer;
static uint32_t s_walk_steps = 0;
static uint32_t s_run_steps = 0;
static bool s_steps_dirty = false;
static uint32_t s_batches_since_save = 0;
#define STEPS_SAVE_PERIOD_BATCHES 120  // ~60s (mỗi batch ~0.5s) → giảm hao mòn NVS

/// Debounce bước: HAR nhầm Trans→Walk chỉ kéo dài 1 cửa sổ (vd "ngồi dậy nhanh") sẽ làm
/// pedometer cộng bước ảo. Vì vậy chỉ chốt bước khi Walk/Run kéo dài liên tục
/// ≥ STEPS_CONFIRM_BATCHES cửa sổ. Bước phát hiện trong lúc chờ xác nhận được GIỮ TẠM
/// (pending); đủ chuỗi → cộng nốt (không mất bước đầu đoạn đi); đứt chuỗi → huỷ (loại flicker).
#define STEPS_CONFIRM_BATCHES 2   // ~1.0s Walk/Run liên tục mới tính là locomotion thật
static uint16_t s_locomotion_streak = 0;
static uint32_t s_pending_walk = 0;
static uint32_t s_pending_run = 0;

/// Nạp số bước tích lũy đã lưu trong NVS (giữ qua reboot/mất điện).
static void steps_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("config", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "walk_steps", &s_walk_steps);
        nvs_get_u32(h, "run_steps", &s_run_steps);
        nvs_close(h);
    }
}

/// Lưu số bước tích lũy vào NVS (gọi định kỳ, chỉ khi có thay đổi).
static void steps_save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("config", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "walk_steps", s_walk_steps);
        nvs_set_u32(h, "run_steps", s_run_steps);
        nvs_commit(h);
        nvs_close(h);
    }
}

/**
 * @brief ISR callback của PCNT: đánh thức task xử lý khi đã đếm đủ IMU_BATCH_SIZE xung INT.
 * @param unit Đơn vị PCNT phát sinh sự kiện.
 * @param edata Dữ liệu sự kiện watch point.
 * @param user_ctx Con trỏ ngữ cảnh người dùng (không dùng).
 * @return true nếu cần yield sang task ưu tiên cao hơn sau khi thoát ISR.
 */
static bool IRAM_ATTR pcnt_on_reach(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    vTaskNotifyGiveFromISR(imu_task_handle, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

/**
 * @brief Task xử lý dữ liệu IMU chính: chờ ngắt PCNT, đọc FIFO theo lô, đổi hệ trục,
 *        lọc Kalman, chuẩn hóa vào sliding window và đẩy sang svc_ai / batch streaming.
 * @param pvParameters Tham số task FreeRTOS (không dùng).
 */
static void imu_processing_task(void *pvParameters)
{
    // Tăng buffer lên 100 (thay vì IMU_BATCH_SIZE=50) để drain sạch FIFO, 
    // tránh lỗi tích tụ (drift) giữa PCNT và số mẫu thực tế gây tràn FIFO.
    mpu6050_data_raw_t raw_data[100];
    uint16_t count;

    // Lấy sample rate thực tế từ driver (tránh hằng số cứng)
    uint16_t sample_rate = mpu6050_get_sample_rate();
    if (sample_rate == 0) sample_rate = 100; // Phòng hờ sample_rate = 0 do lỗi đọc cấu hình
    float dt = 1.0f / (float)sample_rate;

    while (1) {
        /// Self-healing: chờ ngắt PCNT (mỗi ~500ms khi đủ 50 mẫu) với timeout 1s.
        /// Nếu quá hạn → nghi MPU6050 treo FIFO/INT, reset FIFO và bộ đếm để tự phục hồi.
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (notified == 0) {
            s_i2c_consecutive_errors++;
            ESP_LOGW(TAG, "Timeout waiting for PCNT interrupt. Possible MPU6050 FIFO/INT hang. Recovering... (errors: %d)", s_i2c_consecutive_errors);
            if (s_i2c_consecutive_errors >= 10) {
                ESP_LOGE(TAG, "I2C error/timeout threshold reached. Posting SYS_EVT_HARDWARE_ERROR...");
                esp_event_post(SYS_EVENT, SYS_EVT_HARDWARE_ERROR, NULL, 0, portMAX_DELAY);
                s_i2c_consecutive_errors = 0; // Chống spam
            }
            mpu6050_reset_fifo();
            pcnt_unit_clear_count(pcnt_unit);
            continue;
        }

        count = 100; // Đọc tối đa 100 mẫu để vét sạch FIFO
        // Đọc dữ liệu từ FIFO (Burst Read)
        esp_err_t err = mpu6050_read_fifo(raw_data, &count);
        if (err != ESP_OK) {
            s_i2c_consecutive_errors++;
            ESP_LOGE(TAG, "Failed to read MPU6050 FIFO (%s). Consecutive errors: %d", esp_err_to_name(err), s_i2c_consecutive_errors);
            if (s_i2c_consecutive_errors >= 10) {
                ESP_LOGE(TAG, "I2C error/timeout threshold reached. Posting SYS_EVT_HARDWARE_ERROR...");
                esp_event_post(SYS_EVENT, SYS_EVT_HARDWARE_ERROR, NULL, 0, portMAX_DELAY);
                s_i2c_consecutive_errors = 0; // Chống spam
            }
            continue;
        }

        s_i2c_consecutive_errors = 0;
        if (count > 0) {
            system_state_t st = sys_manager_get_state();
            bool is_normal    = (st == STATE_NORMAL);
            bool is_streaming = (st == STATE_STREAMING);
            if (!is_normal && !is_streaming) {
                continue;   // INIT/CONNECTING/OTA: đã drain FIFO, bỏ toàn bộ xử lý nặng
            }

            if (!is_streaming) {
                s_batch_data.count = 0;
            }

            bool is_walk = false;
            bool is_run = false;
            bool steps_confirmed = false;

            if (is_normal) {
                /// Đọc class HAR mới nhất (cập nhật mỗi 0.5s) để gate + gán nhãn bước cho cả lô.
                /// Gait đổi chậm nên dùng chung class cho 50 mẫu là đủ chính xác.
                const char *har_class = svc_ai_get_latest_prediction();
                is_walk = (strcmp(har_class, "Walk") == 0);
                is_run  = (strcmp(har_class, "Run") == 0);
                /// Trơ theo nhịp: chạy (~200ms) nhanh hơn đi bộ (~300ms).
                s_pedometer.refractory_samples = (uint32_t)((is_run ? 0.20f : 0.30f) * sample_rate);

                /// Debounce: cập nhật chuỗi Walk/Run liên tiếp; đủ ngưỡng → chốt nốt bước pending,
                /// đứt chuỗi → huỷ pending (loại bước ảo do HAR nhầm 1 cửa sổ, vd "ngồi dậy nhanh").
                bool is_locomotion = is_walk || is_run;
                if (is_locomotion) {
                    if (s_locomotion_streak < 0xFFFF) s_locomotion_streak++;
                    if (s_locomotion_streak == STEPS_CONFIRM_BATCHES) {
                        // Vừa đủ xác nhận locomotion thật → cộng nốt các bước đã giữ tạm (không mất bước đầu đoạn đi).
                        s_walk_steps += s_pending_walk;
                        s_run_steps  += s_pending_run;
                        if (s_pending_walk || s_pending_run) s_steps_dirty = true;
                        s_pending_walk = 0;
                        s_pending_run  = 0;
                    }
                } else {
                    s_locomotion_streak = 0;
                    s_pending_walk = 0;
                    s_pending_run  = 0;
                }
                steps_confirmed = (s_locomotion_streak >= STEPS_CONFIRM_BATCHES);
            }

            for (int i = 0; i < count; i++) {
                mpu6050_data_t processed_data;
                mpu6050_raw_to_float(&raw_data[i], &processed_data);

                /// Đổi hệ trục: Sensor frame → Body frame (Forward-Left-Up).
                /// Phải đồng nhất với hệ trục dùng khi train model thì inference mới đúng.
                float ax_body = -processed_data.ax;
                float ay_body = -processed_data.ay;
                float az_body = processed_data.az;
                float gx_body = -processed_data.gx;
                float gy_body = -processed_data.gy;
                float gz_body = processed_data.gz;

                if (is_normal) {
                    // Tính toán SVM, theo dõi free-fall và impact thô per-sample
                    int64_t now_us = esp_timer_get_time();
                    int64_t sample_ts = now_us - (count - 1 - i) * (1000000 / sample_rate);

                    float svm = sqrtf(ax_body * ax_body + ay_body * ay_body + az_body * az_body);
                    if (svm < FREEFALL_THR_G) {
                        s_temp_had_freefall = true;
                        s_temp_freefall_ts = sample_ts;
                    }
                    if (s_temp_had_freefall && (sample_ts - s_temp_freefall_ts > 500000)) {
                        s_temp_had_freefall = false;
                    }
                    if (svm > IMPACT_THR_G) {
                        portENTER_CRITICAL(&s_impact_mux);
                        if (sample_ts - s_latest_impact.ts_us < 100000) {
                            if (svm > s_latest_impact.peak_g) {
                                s_latest_impact.peak_g = svm;
                                s_latest_impact.ts_us = sample_ts;
                            }
                            if (s_temp_had_freefall) {
                                s_latest_impact.had_freefall = true;
                            }
                        } else {
                            s_latest_impact.ts_us = sample_ts;
                            s_latest_impact.peak_g = svm;
                            s_latest_impact.had_freefall = s_temp_had_freefall;
                        }
                        portEXIT_CRITICAL(&s_impact_mux);
                    }

                    /// Pedometer ăn accel THÔ body-frame (chưa qua Kalman làm mượt) để giữ
                    /// đỉnh bước rõ; chỉ cộng khi HAR là Walk/Run, gán đúng bộ đếm.
                    if (pedometer_process(&s_pedometer, ax_body, ay_body, az_body)) {
                        /// Đã xác nhận locomotion → cộng thẳng; chưa → giữ tạm chờ debounce xác nhận.
                        if (steps_confirmed) {
                            if (is_run)       { s_run_steps++;  s_steps_dirty = true; }
                            else if (is_walk) { s_walk_steps++; s_steps_dirty = true; }
                        } else {
                            if (is_run)       s_pending_run++;
                            else if (is_walk) s_pending_walk++;
                        }
                    }

                    /// Roll xác định tư thế (nằm/đứng/ngồi) — hợp với mounting thắt lưng phía trước.
                    float accel_roll = atan2(ay_body, sqrt(ax_body * ax_body + az_body * az_body)) * RAD_TO_DEG;
                    last_roll = kalman_get_angle(&kal_roll, accel_roll, gx_body, dt);
                }

                /// Lọc Kalman 1D từng trục IMU rồi chuẩn hóa về (-1, 1):
                /// tiền xử lý bắt buộc để dữ liệu khớp với input của model TinyML đã quantize INT8.
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

                /// Khi STATE_STREAMING: stream phải dùng đúng data đã đổi hệ trục (Body frame)
                /// và đã qua Kalman 1D — tiền xử lý đồng nhất với data inference của TinyML
                /// thì dataset thu được mới khớp với điều kiện chạy thật.
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
            if (is_normal) {
                // Ta gọi svc_ai_process_window() ở đây vì `count` đọc từ FIFO luôn bằng IMU_BATCH_SIZE (50)
                // Vậy cứ qua vòng lặp xử lý 50 mẫu, ta gửi window 1 lần (Trượt 0.5s)
                svc_ai_process_window(&imu_win);
            }

            /// Lưu số bước vào NVS định kỳ (~60s), chỉ khi có thay đổi → bền qua reboot
            /// mà không làm mòn flash.
            if (is_normal) {
                if (++s_batches_since_save >= STEPS_SAVE_PERIOD_BATCHES) {
                    s_batches_since_save = 0;
                    if (s_steps_dirty) { steps_save_nvs(); s_steps_dirty = false; }
                }
            }

            // In log mỗi khi xử lý xong một batch (0.5s)
            //ESP_LOGI(TAG, "Batch processed: %d samples. Current Roll: %.2f", count, last_roll);
        }
    }
}

/**
 * @brief Khởi tạo IMU Service: cấu hình MPU6050, tính dải chuẩn hóa, khởi tạo các bộ lọc
 *        Kalman, hot start, tạo task xử lý và thiết lập PCNT đếm xung INT.
 * @param int_pin Chân GPIO được nối với chân INT của MPU6050.
 * @return ESP_OK nếu khởi tạo thành công.
 */
esp_err_t imu_service_init(gpio_num_t int_pin)
{
    // 1. Cấu hình MPU6050 (tập trung tại đây, không hardcode ngoài app_main)
    mpu6050_config_t imu_cfg = {
        .accel_fs = ACCEL_FS_8G,
        .gyro_fs = GYRO_FS_500DPS,
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
    kalman_init(&kal_roll);
    memset(&imu_win, 0, sizeof(imu_window_t));

    /// HOT START: seed Kalman bằng một mẫu đọc thực ngay lúc khởi tạo, tránh giai đoạn
    /// ramp-up từ 0 (vài giây đầu sai số lớn) trước khi bộ lọc hội tụ.
    mpu6050_data_t init_data = mpu6050_read();
    float ax_body = -init_data.ax;
    float ay_body = -init_data.ay;
    float az_body = init_data.az;

    float init_roll = atan2(ay_body, sqrt(ax_body * ax_body + az_body * az_body)) * RAD_TO_DEG;
    kal_roll.angle = init_roll;
    last_roll = init_roll;

    // Seed 6-axis Kalman 1D với giá trị đọc ban đầu
    kf_ax.x = ax_body;  kf_ay.x = ay_body;  kf_az.x = az_body;
    kf_gx.x = -init_data.gx;  kf_gy.x = -init_data.gy;  kf_gz.x = init_data.gz;

    ESP_LOGI(TAG, "Hot start completed. Initial Roll: %.2f", init_roll);

    /// Reset FIFO trước khi chạy: sau ~3s calibrate, FIFO đã tràn và byte bị lệch khung.
    mpu6050_reset_fifo();

    /// Khởi tạo pedometer (band-pass + peak-detect) và nạp số bước đã lưu trong NVS.
    pedometer_init(&s_pedometer, (float)imu_cfg.sample_rate_hz);
    steps_load_nvs();
    ESP_LOGI(TAG, "Pedometer init. Loaded steps: walk=%lu, run=%lu",
             (unsigned long)s_walk_steps, (unsigned long)s_run_steps);

    // 3. Tạo Task xử lý (độ ưu tiên cao)
    xTaskCreate(imu_processing_task, "imu_task", 4096, NULL, 10, &imu_task_handle);

    /// PCNT offloading: đếm xung INT của IMU bằng phần cứng → CPU chỉ thức dậy mỗi 50 mẫu
    /// (một batch) thay vì 100 lần/giây, giảm đáng kể số lần ngắt và tiết kiệm điện.
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

/**
 * @brief Trả về góc roll mới nhất (đã lọc Kalman) phục vụ xác định tư thế.
 * @param roll Con trỏ nhận giá trị roll (đơn vị độ).
 */
void imu_service_get_latest_roll(float *roll)
{
    *roll = last_roll;
}

/**
 * @brief Đăng ký callback nhận lô dữ liệu IMU đã tiền xử lý khi ở STATE_STREAMING.
 * @param cb Hàm callback được gọi mỗi khi gom đủ IMU_BATCH_SIZE mẫu.
 */
void imu_service_register_batch_callback(imu_batch_callback_t cb)
{
    s_batch_callback = cb;
}

/**
 * @brief Lấy số bước chân tích lũy (đi bộ và chạy) đếm được trên thiết bị.
 * @param walk Con trỏ nhận số bước đi bộ (có thể NULL).
 * @param run  Con trỏ nhận số bước chạy (có thể NULL).
 */
void imu_service_get_steps(uint32_t *walk, uint32_t *run)
{
    if (walk) *walk = s_walk_steps;
    if (run)  *run = s_run_steps;
}

void imu_service_get_last_impact(imu_impact_info_t *out)
{
    if (out) {
        portENTER_CRITICAL(&s_impact_mux);
        *out = s_latest_impact;
        portEXIT_CRITICAL(&s_impact_mux);
    }
}
