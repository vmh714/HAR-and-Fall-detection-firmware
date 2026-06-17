idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES drv_mpu6050 drv_battery svc_imu svc_ai svc_network svc_cloud sys_manager nvs_flash esp_event esp_netif)
