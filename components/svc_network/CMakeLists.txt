idf_component_register(SRCS "svc_network.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi nvs_flash esp_event esp_netif sys_manager
                             esp_modem drv_a7680c driver)
