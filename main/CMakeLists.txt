idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES mpu6050 imu_service wifi_mqtt_service nvs_flash esp_event esp_netif)
