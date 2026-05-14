idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES drv_mpu6050 svc_imu svc_network sys_manager nvs_flash esp_event esp_netif)
