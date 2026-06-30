#include "drv_battery.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "DRV_BATTERY";

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static adc_unit_t s_unit;
static adc_channel_t s_chan;
static float s_ratio = 2.0f;
static bool s_ready = false;

static SemaphoreHandle_t s_batt_mutex = NULL;
static int s_batt_history[5] = {0};
static int s_batt_hist_idx = 0;
static esp_timer_handle_t s_batt_timer = NULL;

static int drv_battery_read_mv_instant(void)
{
    if (!s_ready) return -1;
    int raw = 0;
    if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) return -1;

    int mv = 0;
    if (s_cali) {
        adc_cali_raw_to_voltage(s_cali, raw, &mv);
    } else {
        /// Xấp xỉ khi không có cali: 12-bit (4095), full-scale ~3100mV ở atten 12dB.
        mv = (int)(raw * 3100.0f / 4095.0f);
    }
    return (int)(mv * s_ratio);  // nhân hệ số cầu phân áp → điện áp pin thực
}

static float s_ema_mv = 0;

static void batt_timer_cb(void* arg)
{
    int mv = drv_battery_read_mv_instant();
    if (mv >= 0 && s_batt_mutex) {
        xSemaphoreTake(s_batt_mutex, portMAX_DELAY);
        s_batt_history[s_batt_hist_idx] = mv;
        s_batt_hist_idx = (s_batt_hist_idx + 1) % 5;
        
        // Sắp xếp mảng để lấy trung vị (Median filter)
        int sorted[5];
        for (int i = 0; i < 5; i++) sorted[i] = s_batt_history[i];
        for (int i = 0; i < 4; i++) {
            for (int j = i + 1; j < 5; j++) {
                if (sorted[i] > sorted[j]) {
                    int tmp = sorted[i];
                    sorted[i] = sorted[j];
                    sorted[j] = tmp;
                }
            }
        }
        int median = sorted[2];
        
        // Lọc EMA (Exponential Moving Average) với alpha = 0.1
        if (s_ema_mv == 0) {
            s_ema_mv = median;
        } else {
            s_ema_mv = 0.1f * median + 0.9f * s_ema_mv;
        }
        
        xSemaphoreGive(s_batt_mutex);
    }
}


esp_err_t drv_battery_init(int adc_gpio, float divider_ratio)
{
    s_ratio = divider_ratio;

    /// Suy ra ADC unit + channel từ chân GPIO → đổi pin chỉ cần sửa macro ở hardware_config.
    esp_err_t err = adc_oneshot_io_to_channel(adc_gpio, &s_unit, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d không phải chân ADC hợp lệ", adc_gpio);
        return err;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = s_unit };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    /// Atten 12dB để đo được tới ~3.1V (Vpin/2 của Li-ion ~1.5–2.1V nằm trong tầm).
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg));

    /// Hiệu chuẩn curve-fitting (ESP32-S3) để đổi raw→mV chính xác; nếu không có thì dùng xấp xỉ.
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = s_unit,
        .chan = s_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration không khả dụng — dùng xấp xỉ tuyến tính");
        s_cali = NULL;
    }

    s_ready = true;
    
    s_batt_mutex = xSemaphoreCreateMutex();
    // Khởi tạo lịch sử với giá trị hiện tại để tránh giá trị 0
    int initial_mv = drv_battery_read_mv_instant();
    if (initial_mv < 0) initial_mv = 3700; // fallback
    for (int i = 0; i < 5; i++) {
        s_batt_history[i] = initial_mv;
    }
    s_ema_mv = initial_mv;
    
    const esp_timer_create_args_t timer_args = {
        .callback = &batt_timer_cb,
        .name = "batt_timer"
    };
    esp_timer_create(&timer_args, &s_batt_timer);
    esp_timer_start_periodic(s_batt_timer, 2000000ULL); // 2 giây

    ESP_LOGI(TAG, "Battery ADC init: unit=%d chan=%d gpio=%d ratio=%.2f",
             s_unit, s_chan, adc_gpio, s_ratio);
    return ESP_OK;
}

int drv_battery_read_mv(void)
{
    if (!s_ready || !s_batt_mutex) return -1;
    
    int mv = 0;
    xSemaphoreTake(s_batt_mutex, portMAX_DELAY);
    mv = (int)s_ema_mv;
    xSemaphoreGive(s_batt_mutex);
    
    return mv;
}

static const uint16_t bat_voltage_lut[11] = {
    3000, // 0%
    3500, // 10%
    3600, // 20%
    3680, // 30%
    3740, // 40%
    3770, // 50%
    3800, // 60%
    3850, // 70%
    3950, // 80%
    4050, // 90%
    4200  // 100%
};

static int s_reported_percent = -1;
static int64_t s_higher_percent_start_us = 0;

int drv_battery_read_percent(void)
{
    int mv = drv_battery_read_mv();
    if (mv < 0) return -1;

    int new_pct = 0;
    if (mv >= bat_voltage_lut[10]) {
        new_pct = 100;
    } else if (mv <= bat_voltage_lut[0]) {
        new_pct = 0;
    } else {
        for (int i = 0; i < 10; i++) {
            if (mv >= bat_voltage_lut[i] && mv <= bat_voltage_lut[i+1]) {
                int pct_low = i * 10;
                int pct_high = (i + 1) * 10;
                int v_low = bat_voltage_lut[i];
                int v_high = bat_voltage_lut[i+1];
                
                new_pct = pct_low + (mv - v_low) * (pct_high - pct_low) / (v_high - v_low);
                break;
            }
        }
    }

    // Monotonic discharging logic (Chỉ giảm, không tăng)
    if (s_reported_percent < 0) {
        s_reported_percent = new_pct;
    } else {
        if (new_pct > s_reported_percent) {
            if (s_higher_percent_start_us == 0) {
                s_higher_percent_start_us = esp_timer_get_time();
            } else if (esp_timer_get_time() - s_higher_percent_start_us > 60000000ULL) { // 1 phút
                s_reported_percent = new_pct;
                s_higher_percent_start_us = 0;
            }
        } else if (new_pct < s_reported_percent) {
            s_reported_percent = new_pct;
            s_higher_percent_start_us = 0;
        } else {
            s_higher_percent_start_us = 0;
        }
    }

    return s_reported_percent;
}
