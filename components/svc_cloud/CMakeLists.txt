idf_component_register(SRCS "svc_cloud.c"
                    INCLUDE_DIRS "include"
                    REQUIRES mqtt esp_event sys_manager json mbedtls svc_imu nvs_flash drv_battery lib_tinyml svc_network svc_ota)
