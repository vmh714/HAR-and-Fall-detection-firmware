#include "mpu6050.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *TAG = "MPU6050";

// Cấu trúc nội bộ (chỉ file .c này biết)
typedef struct
{
    i2c_master_dev_handle_t i2c_dev; // Sẽ được cấp phát trong hàm init
    mpu6050_config_t current_config;

    // Biến lưu Offset của Gyro
    int16_t gyro_offset_x;
    int16_t gyro_offset_y;
    int16_t gyro_offset_z;
} mpu6050_device_t;

static mpu6050_device_t mpu6050_dev;

/**
 * @brief Khởi tạo MPU6050: gắn thiết bị vào bus I2C, kiểm tra WHO_AM_I và đánh thức cảm biến.
 * @param bus_handle Handle của bus I2C master đã được khởi tạo trước đó.
 */
void mpu6050_init(i2c_master_bus_handle_t bus_handle)
{
    // 1. Khai báo cấu hình riêng cho thiết bị MPU6050
    i2c_device_config_t imu_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU_ADDR,
        .scl_speed_hz = MPU_FREQ,
    };

    // 2. Ép thiết bị vào Bus, lưu handle trả về vào struct mpu6050_dev nội bộ
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &imu_cfg, &mpu6050_dev.i2c_dev));

    // 3. Tiến hành ping WHO_AM_I và đánh thức cảm biến
    uint8_t data_transmit[2] = {WHO_AM_I};
    uint8_t buffer;

    esp_err_t err = i2c_master_transmit_receive(mpu6050_dev.i2c_dev, data_transmit, 1, &buffer, 1, 100);
    ESP_ERROR_CHECK(err);

    if (buffer == MPU_ADDR)
    {
        ESP_LOGI(TAG, "MPU6050 Init success! WHO_AM_I = 0x%02X", buffer);
    }
    else
    {
        ESP_LOGE(TAG, "MPU6050 Init failed! Expected 0x68, got 0x%02X", buffer);
    }

    data_transmit[0] = PWR_MGMT_1;
    data_transmit[1] = 0x01;
    err = i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100);
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "MPU6050 wake up success, PLL GX are chossen for clock source!");
}

/**
 * @brief Cấu hình MPU6050: dải đo (FS), bộ lọc DLPF, sample rate, quản lý nguồn, ngắt và FIFO.
 * @param mpu6050_cfg Con trỏ tới cấu trúc cấu hình mong muốn.
 */
void mpu6050_config(const mpu6050_config_t *mpu6050_cfg)
{
    uint8_t data_transmit[2];
    // --- 1. Cấu hình cơ bản (FS Range & DLPF) ---
    data_transmit[0] = ACCEL_CONFIG;
    data_transmit[1] = mpu6050_cfg->accel_fs << ACCEL_FS_POS;
    ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));

    data_transmit[0] = GYRO_CONFIG;
    data_transmit[1] = mpu6050_cfg->gyro_fs << GYRO_FS_POS;
    ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));

    data_transmit[0] = MPU_REG_CONFIG;
    data_transmit[1] = mpu6050_cfg->dlpf_cfg;
    ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));

    if (mpu6050_cfg->sample_rate_hz > 0)
    {
        uint16_t gyro_rate = 8000; // Mặc định là 8kHz nếu tắt DLPF (DLPF_CFG = 0 hoặc 7)

        /// Nếu DLPF được bật (từ 1 đến 6), Gyro Output Rate tự động chốt ở 1kHz
        if (mpu6050_cfg->dlpf_cfg > 0 && mpu6050_cfg->dlpf_cfg < 7)
        {
            gyro_rate = 1000;
        }

        /// Áp dụng công thức: SMPLRT_DIV = (Gyro_Rate / Sample_Rate) - 1
        // Dùng biến uint32_t tạm thời để tránh tràn số âm khi tính toán
        uint32_t div = (gyro_rate / mpu6050_cfg->sample_rate_hz) - 1;

        /// BẢO VỆ PHẦN CỨNG: Thanh ghi 0x19 chỉ chứa được 8 bit (Max = 255)
        if (div > 255)
        {
            div = 255;
            ESP_LOGW("MPU Config", "The desired sample size is too low! Minimum value has been forced: %d Hz", gyro_rate / 256);
        }

        uint8_t div_8bit = (uint8_t)div;

        // Ghi xuống thanh ghi 0x19
        data_transmit[0] = MPU_REG_SMPLRT_DIV;
        data_transmit[1] = div_8bit;
        ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));

        ESP_LOGI("MPU Config", "Sample Rate set to: %d Hz (Divider: %d)", gyro_rate / (div_8bit + 1), div_8bit);
    }
    else
    {
        ESP_LOGE("MPU Config", "Error: Sample rate cannot be 0!");
    }

    // --- 2. Cấu hình Quản lý năng lượng (PWR_MGMT) ---

    if (mpu6050_cfg->pwr_cfg.temp_disable)
    {
        // Đọc giá trị hiện tại của PWR_MGMT_1 để tránh ghi đè các bit khác
        uint8_t pwr1_val = 0x01; // Mặc định dùng PLL Clock
        pwr1_val |= (1 << 3);    // Bit TEMP_DIS
        data_transmit[0] = PWR_MGMT_1;
        data_transmit[1] = pwr1_val;
        ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));
    }
    // Chỉ ghi PWR_MGMT_2 nếu bạn muốn đưa Accel hoặc Gyro vào chế độ Standby
    if (mpu6050_cfg->pwr_cfg.accel_standby || mpu6050_cfg->pwr_cfg.gyro_standby)
    {
        uint8_t pwr2_val = 0x00;
        if (mpu6050_cfg->pwr_cfg.accel_standby)
            pwr2_val |= 0x38; // Tắt 3 trục Accel (Bit 5,4,3)
        if (mpu6050_cfg->pwr_cfg.gyro_standby)
            pwr2_val |= 0x07; // Tắt 3 trục Gyro (Bit 2,1,0)
        data_transmit[0] = PWR_MGMT_2;
        data_transmit[1] = pwr2_val;
        ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));
    }

    // --- 3. Cấu hình Ngắt (Interrupt) ---
    if (mpu6050_cfg->int_cfg.data_ready_en || mpu6050_cfg->int_cfg.fifo_overflow_en)
    {
        uint8_t int_pin_cfg = 0x00;
        if (mpu6050_cfg->int_cfg.active_low)
            int_pin_cfg |= (1 << 7);
        if (mpu6050_cfg->int_cfg.open_drain)
            int_pin_cfg |= (1 << 6);
        if (mpu6050_cfg->int_cfg.latch_en)
            int_pin_cfg |= (1 << 5);
        // Cho phép đọc xóa ngắt bằng bất kỳ thanh ghi nào (tùy chọn)
        int_pin_cfg |= (1 << 4);
        data_transmit[0] = INT_PIN_CFG;
        data_transmit[1] = int_pin_cfg;
        ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));
        uint8_t int_enable = 0x00;
        if (mpu6050_cfg->int_cfg.fifo_overflow_en)
            int_enable |= (1 << 4);
        if (mpu6050_cfg->int_cfg.data_ready_en)
            int_enable |= (1 << 0);
        data_transmit[0] = INT_ENABLE;
        data_transmit[1] = int_enable;
        ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));
    }
    // --- 4. Cấu hình FIFO ---
    if (mpu6050_cfg->fifo_cfg.fifo_enable)
    {
        // Bước A: Tắt FIFO và Reset
        data_transmit[0] = USER_CTRL;
        data_transmit[1] = 0x00; // Tạm tắt để cấu hình
        ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));
        data_transmit[1] = (1 << 2); // Reset FIFO
        ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));
        // Bước B: Chọn dữ liệu đẩy vào FIFO
        uint8_t fifo_en_val = 0x00;
        if (mpu6050_cfg->fifo_cfg.temp_fifo_en)
            fifo_en_val |= (1 << 7);
        if (mpu6050_cfg->fifo_cfg.gyro_fifo_en)
            fifo_en_val |= 0x70; // X, Y, Z Gyro
        if (mpu6050_cfg->fifo_cfg.accel_fifo_en)
            fifo_en_val |= (1 << 3); // Accel (cả 3 trục)
        data_transmit[0] = FIFO_EN;
        data_transmit[1] = fifo_en_val;
        ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));
        // Bước C: Bật module FIFO (USER_CTRL) nếu cấu hình yêu cầu
        if (mpu6050_cfg->fifo_cfg.fifo_enable)
        {
            data_transmit[0] = USER_CTRL;
            data_transmit[1] = (1 << 6); // FIFO_EN bit
            ESP_ERROR_CHECK(i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100));
        }
    }
    mpu6050_dev.current_config = *mpu6050_cfg;
}

/**
 * @brief Chuyển dữ liệu thô sang đơn vị vật lý (g, deg/s, °C) dựa trên SSF của cấu hình hiện tại.
 * @param raw  Con trỏ tới dữ liệu thô đầu vào.
 * @param data Con trỏ tới cấu trúc nhận dữ liệu đã quy đổi.
 */
void mpu6050_raw_to_float(const mpu6050_data_raw_t *raw, mpu6050_data_t *data)
{
    float a_ssf = accel_ssf[mpu6050_dev.current_config.accel_fs];
    float g_ssf = gyro_ssf[mpu6050_dev.current_config.gyro_fs];

    data->ax = (float)raw->ax / a_ssf;
    data->ay = (float)raw->ay / a_ssf;
    data->az = (float)raw->az / a_ssf;
    
    /// Công thức tính nhiệt độ chuẩn từ datasheet
    data->temp = (float)raw->temp / 340.0f + 36.53f;

    data->gx = (float)raw->gx / g_ssf;
    data->gy = (float)raw->gy / g_ssf;
    data->gz = (float)raw->gz / g_ssf;
}

/**
 * @brief Lấy sample rate (Hz) đang được cấu hình.
 * @return Tần số lấy mẫu hiện tại tính bằng Hz.
 */
uint16_t mpu6050_get_sample_rate(void)
{
    return mpu6050_dev.current_config.sample_rate_hz;
}

/**
 * @brief Đọc một mẫu dữ liệu thô (Accel/Temp/Gyro) qua I2C; gyro đã được trừ offset tĩnh.
 * @return Cấu trúc dữ liệu thô của một mẫu cảm biến.
 */
mpu6050_data_raw_t mpu6050_read_raw()
{
    uint8_t data_transmit = ACCEL_XOUT_H;
    uint8_t data_raw[14];
    mpu6050_data_raw_t data;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(mpu6050_dev.i2c_dev, &data_transmit, 1, data_raw, 14, 100));

    data.ax = (int16_t)(data_raw[0] << 8 | data_raw[1]);
    data.ay = (int16_t)(data_raw[2] << 8 | data_raw[3]);
    data.az = (int16_t)(data_raw[4] << 8 | data_raw[5]);
    data.temp = (int16_t)(data_raw[6] << 8 | data_raw[7]);

    // Đọc raw
    int16_t raw_gx = (int16_t)(data_raw[8] << 8 | data_raw[9]);
    int16_t raw_gy = (int16_t)(data_raw[10] << 8 | data_raw[11]);
    int16_t raw_gz = (int16_t)(data_raw[12] << 8 | data_raw[13]);

    /// Trừ đi sai số tĩnh (Offset) đã hiệu chỉnh từ mpu6050_calibrate_gyro()
    data.gx = raw_gx - mpu6050_dev.gyro_offset_x;
    data.gy = raw_gy - mpu6050_dev.gyro_offset_y;
    data.gz = raw_gz - mpu6050_dev.gyro_offset_z;

    return data;
}

/**
 * @brief Đọc một mẫu và chuyển sang đơn vị vật lý (g, deg/s, °C).
 * @return Cấu trúc dữ liệu đã quy đổi sang đơn vị thực.
 */
mpu6050_data_t mpu6050_read()
{
    mpu6050_data_raw_t data_raw = mpu6050_read_raw();
    mpu6050_data_t data;
    float a_ssf = accel_ssf[mpu6050_dev.current_config.accel_fs];
    float g_ssf = gyro_ssf[mpu6050_dev.current_config.gyro_fs];
    data.ax = 1.0 * data_raw.ax / a_ssf;
    data.ay = 1.0 * data_raw.ay / a_ssf;
    data.az = 1.0 * data_raw.az / a_ssf;
    data.temp = 1.0 * data_raw.temp / 340 + 36.53;
    data.gx = 1.0 * data_raw.gx / g_ssf;
    data.gy = 1.0 * data_raw.gy / g_ssf;
    data.gz = 1.0 * data_raw.gz / g_ssf;

    return data;
}
/**
 * @brief Đọc dữ liệu raw gyro trực tiếp từ thanh ghi, KHÔNG trừ offset.
 *        Dùng nội bộ cho quá trình calibration.
 * @param gx Con trỏ nhận giá trị thô trục X.
 * @param gy Con trỏ nhận giá trị thô trục Y.
 * @param gz Con trỏ nhận giá trị thô trục Z.
 */
static void mpu6050_read_raw_gyro_no_offset(int16_t *gx, int16_t *gy, int16_t *gz)
{
    uint8_t reg = ACCEL_XOUT_H;
    uint8_t buf[14];
    i2c_master_transmit_receive(mpu6050_dev.i2c_dev, &reg, 1, buf, 14, 100);
    *gx = (int16_t)(buf[8]  << 8 | buf[9]);
    *gy = (int16_t)(buf[10] << 8 | buf[11]);
    *gz = (int16_t)(buf[12] << 8 | buf[13]);
}

/**
 * @brief Hiệu chỉnh (calibrate) offset tĩnh của Gyro khi thiết bị đứng yên.
 *        Lấy mẫu 2 lần (2-pass), loại mẫu nhiễu theo ngưỡng 3-sigma rồi tính mean offset.
 */
void mpu6050_calibrate_gyro(void)
{
    // ----------------------------------------------------------------
    // BƯỚC 0: Reset offset về 0 để tránh ảnh hưởng nếu gọi lại lần 2
    // ----------------------------------------------------------------
    mpu6050_dev.gyro_offset_x = 0;
    mpu6050_dev.gyro_offset_y = 0;
    mpu6050_dev.gyro_offset_z = 0;

    // ----------------------------------------------------------------
    // BƯỚC 1: Tính delay phù hợp với sample_rate_hz thực tế
    //         Nếu sample_rate = 100Hz → delay = 10ms / mẫu
    //         Đảm bảo mỗi lần đọc là một mẫu MỚI, không đọc trùng
    // ----------------------------------------------------------------
    uint16_t sr = mpu6050_dev.current_config.sample_rate_hz;
    if (sr == 0) sr = 100; // Fallback an toàn
    /// +1ms để chắc chắn mẫu mới sẵn sàng, tránh đọc trùng mẫu cũ
    uint32_t delay_ms = (1000U / sr) + 1;

    const int WARMUP_SAMPLES = 50;   /// Bỏ qua mẫu đầu để cảm biến ổn định
    const int NUM_SAMPLES    = 200;  /// Bội số 50, đủ chính xác (noise ~0.004°/s), tiết kiệm ~3.5s

    ESP_LOGI(TAG, "Gyro calibration: %d samples @ %dHz (delay %lums). Hold still!",
             NUM_SAMPLES, sr, (unsigned long)delay_ms);

    // ----------------------------------------------------------------
    // BƯỚC 2: Warm-up — bỏ qua mẫu đầu
    // ----------------------------------------------------------------
    for (int i = 0; i < WARMUP_SAMPLES; i++)
    {
        int16_t gx, gy, gz;
        mpu6050_read_raw_gyro_no_offset(&gx, &gy, &gz);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    // ----------------------------------------------------------------
    // BƯỚC 3: Pass đầu — lấy mẫu thô để tính mean và std deviation
    //         Dùng để phát hiện outlier (mẫu bị nhiễu / rung tay)
    // ----------------------------------------------------------------
    int32_t  sum_gx = 0, sum_gy = 0, sum_gz = 0;

    /// Cấp phát mảng trên Heap để tránh Stack Overflow
    int16_t *samples_x = (int16_t *)malloc(NUM_SAMPLES * sizeof(int16_t));
    int16_t *samples_y = (int16_t *)malloc(NUM_SAMPLES * sizeof(int16_t));
    int16_t *samples_z = (int16_t *)malloc(NUM_SAMPLES * sizeof(int16_t));

    if (!samples_x || !samples_y || !samples_z) {
        ESP_LOGE(TAG, "Memory allocation failed for calibration! Fallback to 0 offset.");
        if (samples_x) free(samples_x);
        if (samples_y) free(samples_y);
        if (samples_z) free(samples_z);
        return;
    }

    for (int i = 0; i < NUM_SAMPLES; i++)
    {
        mpu6050_read_raw_gyro_no_offset(&samples_x[i], &samples_y[i], &samples_z[i]);
        sum_gx += samples_x[i];
        sum_gy += samples_y[i];
        sum_gz += samples_z[i];
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    int32_t mean_x = sum_gx / NUM_SAMPLES;
    int32_t mean_y = sum_gy / NUM_SAMPLES;
    int32_t mean_z = sum_gz / NUM_SAMPLES;

    // ----------------------------------------------------------------
    // BƯỚC 4: Tính variance để xác định ngưỡng outlier (±3 sigma)
    // ----------------------------------------------------------------
    int64_t var_x = 0, var_y = 0, var_z = 0;
    for (int i = 0; i < NUM_SAMPLES; i++)
    {
        int32_t dx = samples_x[i] - mean_x;
        int32_t dy = samples_y[i] - mean_y;
        int32_t dz = samples_z[i] - mean_z;
        var_x += (int64_t)dx * dx;
        var_y += (int64_t)dy * dy;
        var_z += (int64_t)dz * dz;
    }
    /// std_dev tính bằng integer sqrt (dùng threshold bình phương để tránh sqrt)
    /// Ngưỡng = 9 * variance / N (tương đương 3-sigma bình phương)
    int64_t thr_x = 9 * (var_x / NUM_SAMPLES);
    int64_t thr_y = 9 * (var_y / NUM_SAMPLES);
    int64_t thr_z = 9 * (var_z / NUM_SAMPLES);
    /// Đặt mức tối thiểu để tránh thr = 0 khi noise quá nhỏ
    if (thr_x < 100) thr_x = 100;
    if (thr_y < 100) thr_y = 100;
    if (thr_z < 100) thr_z = 100;

    // ----------------------------------------------------------------
    // BƯỚC 5: Pass thứ 2 — tính mean sau khi loại outlier
    // ----------------------------------------------------------------
    int64_t clean_sum_x = 0, clean_sum_y = 0, clean_sum_z = 0;
    int      valid_count = 0;

    for (int i = 0; i < NUM_SAMPLES; i++)
    {
        int32_t dx = (int32_t)samples_x[i] - mean_x;
        int32_t dy = (int32_t)samples_y[i] - mean_y;
        int32_t dz = (int32_t)samples_z[i] - mean_z;
        /// Loại mẫu nếu bất kỳ trục nào vượt ngưỡng 3-sigma
        if ((int64_t)dx*dx > thr_x ||
            (int64_t)dy*dy > thr_y ||
            (int64_t)dz*dz > thr_z)
        {
            continue; // Bỏ qua mẫu bị nhiễu
        }
        clean_sum_x += samples_x[i];
        clean_sum_y += samples_y[i];
        clean_sum_z += samples_z[i];
        valid_count++;
    }

    /// Nếu quá nhiều outlier (thiết bị bị rung quá mạnh) → fallback về mean đơn giản
    if (valid_count < NUM_SAMPLES / 2)
    {
        ESP_LOGW(TAG, "Too many outliers (%d valid / %d). Device may have moved! Using simple mean.",
                 valid_count, NUM_SAMPLES);
        mpu6050_dev.gyro_offset_x = (int16_t)mean_x;
        mpu6050_dev.gyro_offset_y = (int16_t)mean_y;
        mpu6050_dev.gyro_offset_z = (int16_t)mean_z;
    }
    else
    {
        mpu6050_dev.gyro_offset_x = (int16_t)(clean_sum_x / valid_count);
        mpu6050_dev.gyro_offset_y = (int16_t)(clean_sum_y / valid_count);
        mpu6050_dev.gyro_offset_z = (int16_t)(clean_sum_z / valid_count);
        ESP_LOGI(TAG, "Calibration OK. %d/%d samples used (outliers removed).",
                 valid_count, NUM_SAMPLES);
    }

    // Hiển thị offset cuối cùng theo đơn vị LSB và deg/s
    float g_ssf = gyro_ssf[mpu6050_dev.current_config.gyro_fs];
    ESP_LOGI(TAG, "Offset (LSB): X=%d, Y=%d, Z=%d",
             mpu6050_dev.gyro_offset_x,
             mpu6050_dev.gyro_offset_y,
             mpu6050_dev.gyro_offset_z);
    ESP_LOGI(TAG, "Offset (deg/s): X=%.3f, Y=%.3f, Z=%.3f",
             mpu6050_dev.gyro_offset_x / g_ssf,
             mpu6050_dev.gyro_offset_y / g_ssf,
             mpu6050_dev.gyro_offset_z / g_ssf);

    // Giải phóng bộ nhớ
    free(samples_x);
    free(samples_y);
    free(samples_z);
}

/**
 * @brief Reset bộ đệm FIFO của MPU6050 bằng cách bật bit FIFO_RESET trong thanh ghi USER_CTRL.
 */
void mpu6050_reset_fifo(void)
{
    uint8_t data_transmit[2] = {USER_CTRL, 0x00};
    // Đọc giá trị hiện tại của USER_CTRL
    i2c_master_transmit_receive(mpu6050_dev.i2c_dev, &data_transmit[0], 1, &data_transmit[1], 1, 100);
    // Bật bit 2 (FIFO_RESET)
    data_transmit[1] |= (1 << 2);
    i2c_master_transmit(mpu6050_dev.i2c_dev, data_transmit, 2, 100);
    vTaskDelay(pdMS_TO_TICKS(10)); // Đợi reset hoàn tất
}

/**
 * @brief Đọc nhiều gói tin (packet) từ FIFO và phân giải vào mảng dữ liệu thô.
 * @param data_array Mảng nhận dữ liệu thô đã phân giải.
 * @param count      Vào: số gói tin tối đa muốn đọc; Ra: số gói tin thực tế đã đọc.
 * @return ESP_OK nếu thành công; mã lỗi esp_err_t tương ứng nếu thất bại.
 */
esp_err_t mpu6050_read_fifo(mpu6050_data_raw_t *data_array, uint16_t *count)
{
    // 1. Tính toán kích thước 1 gói tin (Packet Size)
    uint8_t packet_size = 0;
    if (mpu6050_dev.current_config.fifo_cfg.accel_fifo_en)
        packet_size += 6;
    if (mpu6050_dev.current_config.fifo_cfg.gyro_fifo_en)
        packet_size += 6;
    if (mpu6050_dev.current_config.fifo_cfg.temp_fifo_en)
        packet_size += 2;

    if (packet_size == 0)
        return ESP_ERR_INVALID_STATE;

    // 2. Đọc xem hiện tại FIFO đang có bao nhiêu byte
    uint8_t count_reg = FIFO_COUNT_H;
    uint8_t count_data[2];
    ESP_ERROR_CHECK(i2c_master_transmit_receive(mpu6050_dev.i2c_dev, &count_reg, 1, count_data, 2, 100));
    uint16_t bytes_in_fifo = (count_data[0] << 8) | count_data[1];

    /// Phát hiện FIFO Tràn (Overflow) gây lệch byte (out-of-sync): FIFO đầy 1024 byte → reset
    if (bytes_in_fifo == 1024)
    {
        ESP_LOGW("MPU6050", "FIFO Overflow detected! Resetting FIFO to prevent byte misalignment.");
        mpu6050_reset_fifo();
        *count = 0;
        return ESP_OK;
    }
    /// Phát hiện FIFO bị lệch byte (không chia hết cho packet_size) do đọc sót → reset
    else if (bytes_in_fifo % packet_size != 0)
    {
        ESP_LOGW("MPU6050", "FIFO Misaligned (bytes: %d, packet: %d). Resetting...", bytes_in_fifo, packet_size);
        mpu6050_reset_fifo();
        *count = 0;
        return ESP_OK;
    }

    // 3. Tính số gói tin thực tế có thể đọc
    uint16_t pkgs_available = bytes_in_fifo / packet_size;
    if (pkgs_available == 0)
    {
        *count = 0;
        return ESP_OK;
    }

    // Giới hạn số gói tin đọc theo yêu cầu của người dùng
    uint16_t pkgs_to_read = (pkgs_available > *count) ? *count : pkgs_available;
    uint16_t total_bytes = pkgs_to_read * packet_size;

    // 4. Burst Read từ thanh ghi FIFO_R_W
    uint8_t fifo_reg = FIFO_R_W;
    // Cấp phát bộ đệm tạm thời (Lưu ý: ESP32-S3 có đủ RAM cho việc này)
    uint8_t *buffer = (uint8_t *)malloc(total_bytes);
    if (buffer == NULL)
        return ESP_ERR_NO_MEM;

    esp_err_t err = i2c_master_transmit_receive(mpu6050_dev.i2c_dev, &fifo_reg, 1, buffer, total_bytes, 500);
    if (err != ESP_OK)
    {
        free(buffer);
        return err;
    }

    // 5. Phân giải (Parse) dữ liệu từ buffer vào mảng data_raw
    uint16_t offset = 0;
    for (int i = 0; i < pkgs_to_read; i++)
    {
        if (mpu6050_dev.current_config.fifo_cfg.accel_fifo_en)
        {
            data_array[i].ax = (int16_t)(buffer[offset] << 8 | buffer[offset + 1]);
            data_array[i].ay = (int16_t)(buffer[offset + 2] << 8 | buffer[offset + 3]);
            data_array[i].az = (int16_t)(buffer[offset + 4] << 8 | buffer[offset + 5]);
            offset += 6;
        }
        if (mpu6050_dev.current_config.fifo_cfg.temp_fifo_en)
        {
            data_array[i].temp = (int16_t)(buffer[offset] << 8 | buffer[offset + 1]);
            offset += 2;
        }
        if (mpu6050_dev.current_config.fifo_cfg.gyro_fifo_en)
        {
            data_array[i].gx = (int16_t)(buffer[offset] << 8 | buffer[offset + 1]);
            data_array[i].gy = (int16_t)(buffer[offset + 2] << 8 | buffer[offset + 3]);
            data_array[i].gz = (int16_t)(buffer[offset + 4] << 8 | buffer[offset + 5]);
            offset += 6;

            /// Trừ đi Offset sau khi đọc từ FIFO (vì FIFO lưu dữ liệu thô từ cảm biến)
            data_array[i].gx -= mpu6050_dev.gyro_offset_x;
            data_array[i].gy -= mpu6050_dev.gyro_offset_y;
            data_array[i].gz -= mpu6050_dev.gyro_offset_z;
        }
    }

    *count = pkgs_to_read;
    free(buffer);
    return ESP_OK;
}
