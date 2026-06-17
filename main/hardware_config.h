#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

// ======================== NETWORK MODE ========================
// Bỏ comment dòng dưới để build bản PRODUCTION dùng 4G LTE (PPPoS) làm gateway.
// Để nguyên (comment) thì dùng WiFi STA cho môi trường DEV.
// #define NETWORK_USE_CELLULAR 1

// ======================== WIFI CONFIGURATION ========================
#define CONFIG_WIFI_SSID "MD_LAPTOP"
#define CONFIG_WIFI_PASSWORD "11111111"

// ======================== 4G LTE (A7680C) APN ========================
#define A7680C_APN "v-internet"   // Viettel
#define A7680C_APN_USER ""
#define A7680C_APN_PASS ""

// ======================== MQTT CONFIGURATION ========================
#define CONFIG_MQTT_BROKER_URI "mqtt://mqtt.toolhub.app"
#define CONFIG_MQTT_USERNAME "hungvm"
#define CONFIG_MQTT_PASSWORD "20225198"
#define CONFIG_DEVICE_ID "esp32_eldercare_01"

// ======================== GPIO ROUTING ========================

// MPU6050 (I2C & Interrupt)
#define I2C_MASTER_SCL_IO GPIO_NUM_9  // Physical pin D10
#define I2C_MASTER_SDA_IO GPIO_NUM_8  // Physical pin D9
#define MPU6050_INT_PIN GPIO_NUM_7    // Physical pin D8
#define I2C_PORT I2C_NUM_0

// A7680C (4G LTE - UART)
#define A7680C_TX_PIN GPIO_NUM_43
#define A7680C_RX_PIN GPIO_NUM_44
#define A7680C_PWRKEY_PIN GPIO_NUM_38
#define UART_PORT UART_NUM_2
#endif  // HARDWARE_CONFIG_H
