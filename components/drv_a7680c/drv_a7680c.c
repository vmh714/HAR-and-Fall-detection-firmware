#include "drv_a7680c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "DRV_A7680C";

/// Lưu chân PWRKEY sau init; -1 nghĩa là chưa khởi tạo.
static gpio_num_t s_pwrkey_pin = -1;

void drv_a7680c_init(gpio_num_t pwrkey_pin)
{
    s_pwrkey_pin = pwrkey_pin;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pwrkey_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /// Mức cao = trạng thái nghỉ (không kích PWRKEY). Chỉ xung mức thấp mới bật/tắt
    /// module. Lưu ý: cực tính phụ thuộc mạch driver PWRKEY trên board — nếu board
    /// đảo mức (GPIO cao mới kéo PWRKEY xuống) thì hoán đổi 0/1 ở đây.
    gpio_set_level(pwrkey_pin, 1);
    ESP_LOGI(TAG, "PWRKEY init on GPIO%d (idle high)", pwrkey_pin);
}

esp_err_t drv_a7680c_power_on(void)
{
    if (s_pwrkey_pin < 0) return ESP_ERR_INVALID_STATE;

    /// A7680C bật nguồn bằng xung PWRKEY mức thấp NGẮN (Ton ~50ms typical theo
    /// datasheet — nhanh hơn nhiều so với dòng module cũ). Dùng 100ms cho biên an
    /// toàn rồi nhả về cao; sau đó module cần vài giây để boot xong UART/AT.
    ESP_LOGI(TAG, "Power ON: PWRKEY low 100ms...");
    gpio_set_level(s_pwrkey_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(s_pwrkey_pin, 1);
    return ESP_OK;
}

esp_err_t drv_a7680c_power_off(void)
{
    if (s_pwrkey_pin < 0) return ESP_ERR_INVALID_STATE;

    /// Tắt nguồn: giữ PWRKEY mức thấp >= 2.5s (Toff min) theo timing power-down của
    /// A7680C. Lưu ý: phải chờ >= 2s (Toff-on) trước khi power_on() lại để tránh boot lỗi.
    ESP_LOGI(TAG, "Power OFF: PWRKEY low 2.5s...");
    gpio_set_level(s_pwrkey_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(2500));
    gpio_set_level(s_pwrkey_pin, 1);
    return ESP_OK;
}
