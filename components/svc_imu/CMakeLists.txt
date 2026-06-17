idf_component_register(
    SRCS "imu_service.c"
    INCLUDE_DIRS "include"
    REQUIRES drv_mpu6050 lib_kalman sys_manager driver svc_ai lib_pedometer nvs_flash
)