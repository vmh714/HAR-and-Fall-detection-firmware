#include "drv_a7680c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include <string.h>

static const char *TAG = "DRV_A7680C";

static gpio_num_t s_rst_pin = -1;

void drv_a7680c_init(gpio_num_t rst_pin)
{
    s_rst_pin = rst_pin;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << rst_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /// Bỏ qua set idle high vì mạch của user bị tắt ngúm nếu đụng vào chân RST
    // gpio_set_level(rst_pin, 1);
    ESP_LOGI(TAG, "RST init bypassed (to prevent module power-off issue)");
}

esp_err_t drv_a7680c_reset(bool *did_reset)
{
    if (s_rst_pin < 0) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Bỏ qua Hardware Reset để tránh mạch 4G bị tắt hẳn (board không có auto-power-on sau reset)...");
    // gpio_set_level(s_rst_pin, 0);
    // vTaskDelay(pdMS_TO_TICKS(250));
    // gpio_set_level(s_rst_pin, 1);
    
    if (did_reset) {
        *did_reset = false; // Báo cho Caller biết là không hề reset
    }
    
    return ESP_OK;
}

// Bổ sung các API hỗ trợ AT Command khẩn cấp
// Sử dụng hàm esp_modem_at để truyền lệnh trực tiếp khi PPPoS đứt
esp_err_t drv_a7680c_emergency_mqtt_publish(void *dce_ptr, const char *broker, const char *topic, const char *payload)
{
    if (!dce_ptr || !broker || !topic || !payload) return ESP_FAIL;
    esp_modem_dce_t *dce = (esp_modem_dce_t *)dce_ptr;
    char at_out[128] = {0};
    char cmd[256];
    
    ESP_LOGW(TAG, "EMERGENCY: Kích hoạt mạch MQTT nội bộ của A7680C...");
    esp_modem_at(dce, "AT+CMQTTSTART", at_out, 2000);
    esp_modem_at(dce, "AT+CMQTTACCQ=0,\"ESP32_EMG\",0", at_out, 2000);
    
    snprintf(cmd, sizeof(cmd), "AT+CMQTTCONNECT=0,\"%s\",60,1", broker);
    if (esp_modem_at(dce, cmd, at_out, 10000) != ESP_OK) {
        ESP_LOGE(TAG, "EMG: Kết nối broker thất bại");
        return ESP_FAIL;
    }
    
    snprintf(cmd, sizeof(cmd), "AT+CMQTTTOPIC=0,%d", strlen(topic));
    esp_modem_at(dce, cmd, at_out, 2000);
    esp_modem_at(dce, topic, at_out, 2000);
    
    snprintf(cmd, sizeof(cmd), "AT+CMQTTPAYLOAD=0,%d", strlen(payload));
    esp_modem_at(dce, cmd, at_out, 2000);
    esp_modem_at(dce, payload, at_out, 2000);
    
    ESP_LOGI(TAG, "EMG: Publishing payload...");
    esp_modem_at(dce, "AT+CMQTTPUB=0,1,60", at_out, 5000);
    
    // Cleanup
    esp_modem_at(dce, "AT+CMQTTDISC=0,60", at_out, 2000);
    esp_modem_at(dce, "AT+CMQTTSTOP", at_out, 2000);
    
    return ESP_OK;
}
