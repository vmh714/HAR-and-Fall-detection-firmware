idf_component_register(SRCS "wifi_mqtt_service.c"
                    INCLUDE_DIRS "include"
                    REQUIRES mqtt esp_wifi nvs_flash esp_event esp_netif)
