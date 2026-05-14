#ifndef _MPU6050_H_
#define _MPU6050_H_
#include "driver/i2c_master.h"

// define reg
#define MPU_ADDR 0x68
#define MPU_FREQ 400000
#define WHO_AM_I 0x75
#define PWR_MGMT_1 0x6B
#define PWR_MGMT_2 0x6C
#define GYRO_CONFIG 0x1B
#define ACCEL_CONFIG 0x1C
#define ACCEL_XOUT_H 0x3B
#define ACCEL_XOUT_L 0x3C
#define MPU_REG_CONFIG 0x1A
#define MPU_REG_SMPLRT_DIV 0x19
// interupt and fifo reg
#define INT_PIN_CFG 0x37
#define INT_ENABLE 0x38
#define INT_STATUS 0x3A
#define USER_CTRL 0x6A
#define FIFO_EN 0x23
#define FIFO_COUNT_H 0x72
#define FIFO_COUNT_L 0x73
#define FIFO_R_W 0x74

// bitwise
#define ACCEL_FS_POS 3
#define GYRO_FS_POS 3

typedef enum
{
    GYRO_FS_250DPS,
    GYRO_FS_500DPS,
    GYRO_FS_1000DPS,
    GYRO_FS_2000DPS
} gyro_fs_t;

typedef enum
{
    ACCEL_FS_2G,
    ACCEL_FS_4G,
    ACCEL_FS_8G,
    ACCEL_FS_16G
} accel_fs_t;

typedef enum
{
    DLPF_CFG_260HZ = 0,
    DLPF_CFG_184HZ = 1,
    DLPF_CFG_94HZ = 2,
    DLPF_CFG_44HZ = 3,
    DLPF_CFG_21HZ = 4,
    DLPF_CFG_10HZ = 5,
    DLPF_CFG_5HZ = 6,
    DLPF_CFG_REVERSED = 7
} mpu6050_dlpf_t;

static const float gyro_ssf[] = {
    [GYRO_FS_250DPS] = 131.0f,
    [GYRO_FS_500DPS] = 65.5f,
    [GYRO_FS_1000DPS] = 32.8f,
    [GYRO_FS_2000DPS] = 16.4f};
static const float accel_ssf[] = {
    [ACCEL_FS_2G] = 16384.0f,
    [ACCEL_FS_4G] = 8192.0f,
    [ACCEL_FS_8G] = 4096.0f,
    [ACCEL_FS_16G] = 2048.0f};

// --- Cấu hình quản lý năng lượng (Power Management) ---
typedef struct
{
    bool temp_disable;  // true: Tắt cảm biến nhiệt (Bit TEMP_DIS trong thanh ghi PWR_MGMT_1)
    bool gyro_standby;  // true: Đưa cả 3 trục Gyro vào chế độ Sleep (Thanh ghi PWR_MGMT_2)
    bool accel_standby; // true: Đưa cả 3 trục Accel vào chế độ Sleep (Thanh ghi PWR_MGMT_2)
} mpu6050_pwr_cfg_t;

// --- Cấu hình FIFO ---
typedef struct
{
    bool fifo_enable;   // true: Kích hoạt module FIFO (Bit FIFO_EN trong thanh ghi USER_CTRL)
    bool temp_fifo_en;  // true: Đẩy dữ liệu Temp vào FIFO (Thanh ghi FIFO_EN)
    bool gyro_fifo_en;  // true: Đẩy dữ liệu Gyro (X, Y, Z) vào FIFO
    bool accel_fifo_en; // true: Đẩy dữ liệu Accel (X, Y, Z) vào FIFO
} mpu6050_fifo_cfg_t;
typedef struct
{
    bool fifo_overflow_en; // Ngắt khi FIFO tràn (Bit 4 - INT_ENABLE)
    bool data_ready_en;    // Ngắt khi có dữ liệu mới (Bit 0 - INT_ENABLE)

    // Cấu hình mức logic cho chân INT vật lý (Thanh ghi 0x37)
    bool active_low; // true: Chân INT mức thấp khi có ngắt, false: mức cao
    bool open_drain; // Kiểu chân Output (Push-pull hoặc Open-drain)
    bool latch_en;   // true: Giữ chân ngắt cho đến khi được xóa thủ công
} mpu6050_int_cfg_t;
typedef struct
{
    // Cấu hình cơ bản
    accel_fs_t accel_fs;
    gyro_fs_t gyro_fs;
    mpu6050_dlpf_t dlpf_cfg;
    uint16_t sample_rate_hz;

    // Cấu hình nguồn và cảm biến
    mpu6050_pwr_cfg_t pwr_cfg;
    // Cấu hình ngắt
    mpu6050_int_cfg_t int_cfg;
    // Cấu hình bộ đệm dữ liệu
    mpu6050_fifo_cfg_t fifo_cfg;
} mpu6050_config_t;

typedef struct
{
    int16_t ax, ay, az;
    int16_t temp;
    int16_t gx, gy, gz;
} mpu6050_data_raw_t;

typedef struct
{
    float ax, ay, az;
    float temp;
    float gx, gy, gz;
} mpu6050_data_t;

void mpu6050_init();
void mpu6050_config(const mpu6050_config_t *mpu6050_cfg);
mpu6050_data_raw_t mpu6050_read_raw();
mpu6050_data_t mpu6050_read();
void mpu6050_raw_to_float(const mpu6050_data_raw_t *raw, mpu6050_data_t *data);
void mpu6050_reset_fifo(void);
uint16_t mpu6050_get_sample_rate(void);

void mpu6050_calibrate_gyro(void);
esp_err_t mpu6050_read_fifo(mpu6050_data_raw_t *data_array, uint16_t *count);

#endif //_MPU6050_H_