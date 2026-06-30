#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

// ======================== NETWORK MODE ========================
// Chế độ mạng giờ đây tự động: ưu tiên 4G LTE, nếu UART không phản hồi sẽ
// fallback qua WiFi.

// ======================== WIFI CONFIGURATION ========================
#define CONFIG_WIFI_SSID "IT Hoc Bach Khoa"
#define CONFIG_WIFI_PASSWORD "chungtalamotgiadinh"

// ======================== 4G LTE (A7680C) APN ========================
#define A7680C_APN "v-internet"  // Viettel
#define A7680C_APN_USER ""
#define A7680C_APN_PASS ""

// ======================== MQTT CONFIGURATION ========================
#define CONFIG_MQTT_BROKER_URI "mqtts://mqtt.toolhub.app:8883"
#define CONFIG_MQTT_USERNAME "hungvm"
#define CONFIG_MQTT_PASSWORD "20225198"
#define CONFIG_DEVICE_ID "esp32_eldercare_01"

/// Let's Encrypt E8 intermediate CA — ký trực tiếp cho mqtt.toolhub.app (hết
/// hạn 2027-03-12)
#define CONFIG_MQTT_CA_CERT                                              \
    "-----BEGIN CERTIFICATE-----\n"                                      \
    "MIIEVjCCAj6gAwIBAgIQY5WTY8JOcIJxWRi/w9ftVjANBgkqhkiG9w0BAQsFADBP\n" \
    "MQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFy\n" \
    "Y2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMTAeFw0yNDAzMTMwMDAwMDBa\n" \
    "Fw0yNzAzMTIyMzU5NTlaMDIxCzAJBgNVBAYTAlVTMRYwFAYDVQQKEw1MZXQncyBF\n" \
    "bmNyeXB0MQswCQYDVQQDEwJFODB2MBAGByqGSM49AgEGBSuBBAAiA2IABNFl8l7c\n" \
    "S7QMApzSsvru6WyrOq44ofTUOTIzxULUzDMMNMchIJBwXOhiLxxxs0LXeb5GDcHb\n" \
    "R6EToMffgSZjO9SNHfY9gjMy9vQr5/WWOrQTZxh7az6NSNnq3u2ubT6HTKOB+DCB\n" \
    "9TAOBgNVHQ8BAf8EBAMCAYYwHQYDVR0lBBYwFAYIKwYBBQUHAwIGCCsGAQUFBwMB\n" \
    "MBIGA1UdEwEB/wQIMAYBAf8CAQAwHQYDVR0OBBYEFI8NE6L2Ln7RUGwzGDhdWY4j\n" \
    "cpHKMB8GA1UdIwQYMBaAFHm0WeZ7tuXkAXOACIjIGlj26ZtuMDIGCCsGAQUFBwEB\n" \
    "BCYwJDAiBggrBgEFBQcwAoYWaHR0cDovL3gxLmkubGVuY3Iub3JnLzATBgNVHSAE\n" \
    "DDAKMAgGBmeBDAECATAnBgNVHR8EIDAeMBygGqAYhhZodHRwOi8veDEuYy5sZW5j\n" \
    "ci5vcmcvMA0GCSqGSIb3DQEBCwUAA4ICAQBnE0hGINKsCYWi0Xx1ygxD5qihEjZ0\n" \
    "RI3tTZz1wuATH3ZwYPIp97kWEayanD1j0cDhIYzy4CkDo2jB8D5t0a6zZWzlr98d\n" \
    "AQFNh8uKJkIHdLShy+nUyeZxc5bNeMp1Lu0gSzE4McqfmNMvIpeiwWSYO9w82Ob8\n" \
    "otvXcO2JUYi3svHIWRm3+707DUbL51XMcY2iZdlCq4Wa9nbuk3WTU4gr6LY8MzVA\n" \
    "aDQG2+4U3eJ6qUF10bBnR1uuVyDYs9RhrwucRVnfuDj29CMLTsplM5f5wSV5hUpm\n" \
    "Uwp/vV7M4w4aGunt74koX71n4EdagCsL/Yk5+mAQU0+tue0JOfAV/R6t1k+Xk9s2\n" \
    "HMQFeoxppfzAVC04FdG9M+AC2JWxmFSt6BCuh3CEey3fE52Qrj9YM75rtvIjsm/1\n" \
    "Hl+u//Wqxnu1ZQ4jpa+VpuZiGOlWrqSP9eogdOhCGisnyewWJwRQOqK16wiGyZeR\n" \
    "xs/Bekw65vwSIaVkBruPiTfMOo0Zh4gVa8/qJgMbJbyrwwG97z/PRgmLKCDl8z3d\n" \
    "tA0Z7qq7fta0Gl24uyuB05dqI5J1LvAzKuWdIjT1tP8qCoxSE/xpix8hX2dt3h+/\n" \
    "jujUgFPFZ0EVZ0xSyBNRF3MboGZnYXFUxpNjTWPKpagDHJQmqrAcDmWJnMsFY3jS\n" \
    "u1igv3OefnWjSQ==\n"                                                 \
    "-----END CERTIFICATE-----\n"

// ======================== GPIO ROUTING ========================

// MPU6050 (I2C & Interrupt)
#define I2C_MASTER_SCL_IO GPIO_NUM_9  // Physical pin D10
#define I2C_MASTER_SDA_IO GPIO_NUM_8  // Physical pin D9
#define MPU6050_INT_PIN GPIO_NUM_7    // Physical pin D8
#define I2C_PORT I2C_NUM_0

// A7680C (4G LTE - UART)
#define A7680C_TX_PIN GPIO_NUM_5
#define A7680C_RX_PIN GPIO_NUM_4
#define A7680C_RST_PIN GPIO_NUM_6
#define UART_PORT UART_NUM_2

// Battery monitor — ADC đọc qua cầu phân áp 2× 100kΩ (Vpin = Vadc × 2)
#define BATTERY_ADC_GPIO GPIO_NUM_1
#define BATTERY_DIVIDER_RATIO 2.0f
#endif  // HARDWARE_CONFIG_H
