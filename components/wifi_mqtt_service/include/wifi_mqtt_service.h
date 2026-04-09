#ifndef WIFI_MQTT_SERVICE_H
#define WIFI_MQTT_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Cấu hình WiFi + MQTT
 */
typedef struct {
    const char *wifi_ssid;
    const char *wifi_pass;
    const char *mqtt_broker_uri;
    const char *mqtt_username;
    const char *mqtt_password;
    const char *mqtt_client_id;
} wifi_mqtt_config_t;

/**
 * @brief Khởi tạo toàn bộ hạ tầng mạng: NVS → WiFi STA → chờ kết nối → MQTT client.
 *        Hàm này sẽ BLOCK cho đến khi WiFi kết nối thành công.
 * 
 * @param config Cấu hình WiFi + MQTT
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t wifi_mqtt_service_init(const wifi_mqtt_config_t *config);

/**
 * @brief Kiểm tra MQTT client đã kết nối tới broker chưa
 * @return true nếu đã kết nối
 */
bool wifi_mqtt_is_connected(void);

/**
 * @brief Publish message lên MQTT broker
 * 
 * @param topic Topic cần publish
 * @param data  Chuỗi payload (null-terminated)
 * @param qos   QoS level (0, 1, 2)
 * @param retain Retain flag
 * @return int  Message ID nếu thành công, -1 nếu lỗi
 */
int wifi_mqtt_publish(const char *topic, const char *data, int qos, int retain);

/**
 * @brief Lấy Device ID (client_id) đã cấu hình
 * @return Chuỗi client_id
 */
const char *wifi_mqtt_get_device_id(void);

#endif // WIFI_MQTT_SERVICE_H
