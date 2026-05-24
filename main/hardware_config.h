#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

// ======================== WIFI CONFIGURATION ========================
#define CONFIG_WIFI_SSID        "IT Hoc Bach Khoa"
#define CONFIG_WIFI_PASSWORD    "chungtalamotgiadinh"

// ======================== MQTT CONFIGURATION ========================
#define CONFIG_MQTT_BROKER_URI  "mqtt://mqtt.toolhub.app"
#define CONFIG_MQTT_USERNAME    "hungvm"
#define CONFIG_MQTT_PASSWORD    "20225198"
#define CONFIG_DEVICE_ID        "esp32_eldercare_01"

// ======================== GPIO ROUTING ========================

// MPU6050 (I2C & Interrupt)
#define I2C_MASTER_SCL_IO       GPIO_NUM_9
#define I2C_MASTER_SDA_IO       GPIO_NUM_10
#define MPU6050_INT_PIN         GPIO_NUM_11
#define I2C_PORT                I2C_NUM_0

// A7680C (4G LTE - UART)
#define A7680C_TX_PIN           GPIO_NUM_43
#define A7680C_RX_PIN           GPIO_NUM_44
#define A7680C_PWRKEY_PIN       GPIO_NUM_38
#define UART_PORT               UART_NUM_2
#endif // HARDWARE_CONFIG_H
