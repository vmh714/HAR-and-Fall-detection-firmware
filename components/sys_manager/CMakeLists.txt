idf_component_register(
    SRCS "sys_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event svc_cloud esp_timer
)
