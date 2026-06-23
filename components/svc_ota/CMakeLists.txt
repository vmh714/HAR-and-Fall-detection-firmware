idf_component_register(SRCS "svc_ota.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_client app_update sys_manager freertos)
