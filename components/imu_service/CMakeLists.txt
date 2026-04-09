idf_component_register(
    SRCS "imu_service.c"
    INCLUDE_DIRS "include"
    REQUIRES mpu6050 kalman_filter
)