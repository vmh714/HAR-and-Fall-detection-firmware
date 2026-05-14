#include "drv_a7680c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "A7680C_DRV";

void drv_a7680c_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << A7680C_RESET_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(A7680C_RESET_PIN, 1);
}

void drv_a7680c_reset(void) {
    ESP_LOGI(TAG, "Resetting A7680C module...");
    gpio_set_level(A7680C_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(A7680C_RESET_PIN, 1);
}

void drv_a7680c_power_on(void) {
    // Logic for power key if needed, or just reset
    drv_a7680c_reset();
}
