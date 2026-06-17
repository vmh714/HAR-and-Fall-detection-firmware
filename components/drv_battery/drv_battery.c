#include "drv_battery.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "DRV_BATTERY";

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static adc_unit_t s_unit;
static adc_channel_t s_chan;
static float s_ratio = 2.0f;
static bool s_ready = false;

/// Ngưỡng điện áp pin Li-ion 1 cell (mV) để ánh xạ %.
#define BATT_MV_FULL 4200
#define BATT_MV_EMPTY 3300

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
    ESP_LOGI(TAG, "Battery ADC init: unit=%d chan=%d gpio=%d ratio=%.2f",
             s_unit, s_chan, adc_gpio, s_ratio);
    return ESP_OK;
}

int drv_battery_read_mv(void)
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

int drv_battery_read_percent(void)
{
    int mv = drv_battery_read_mv();
    if (mv < 0) return -1;
    /// Ánh xạ tuyến tính tạm thời (Li-ion phi tuyến — tinh chỉnh bằng bảng tra sau nếu cần).
    if (mv >= BATT_MV_FULL) return 100;
    if (mv <= BATT_MV_EMPTY) return 0;
    return (int)((long)(mv - BATT_MV_EMPTY) * 100 / (BATT_MV_FULL - BATT_MV_EMPTY));
}
