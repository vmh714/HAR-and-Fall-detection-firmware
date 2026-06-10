# Component: drv_mpu6050

## Tổng Quan
Driver giao tiếp trực tiếp với cảm biến IMU MPU6050 qua chuẩn I2C. Chịu trách nhiệm thiết lập các thanh ghi, định cấu hình dải đo gia tốc kế/con quay hồi chuyển, và kích hoạt ngắt FIFO tự động.

## Các Struct Chính
```c
typedef struct {
    int16_t accel_x, accel_y, accel_z;
    int16_t gyro_x, gyro_y, gyro_z;
} mpu6050_data_raw_t;
```

## Public API
- `esp_err_t mpu6050_read_fifo(mpu6050_data_raw_t *data_array, uint16_t *count)`: Đọc mảng dữ liệu từ FIFO hardware của MPU6050.
- `void mpu6050_reset_fifo(void)`: Reset FIFO hardware khi có lỗi chống tràn (Watchdog timeout).
- `uint16_t mpu6050_get_sample_rate(void)`: Lấy tần số lấy mẫu (ví dụ 100Hz).
- `void mpu6050_calibrate_gyro(void)`: Tự động đo bias của Gyro lúc khởi động tĩnh để bù trừ góc.

*Hoạt động*: Giao tiếp I2C thuần túy, chỉ trả về dữ liệu byte/int16 thô. Mọi việc nhân scale (g, dps) được thực hiện ở lớp Service để giữ driver nhẹ nhất có thể.
