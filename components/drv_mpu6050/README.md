# Component: drv_mpu6050

> **Cập nhật cuối (Timestamp)**: 2026-06-17

## Tổng Quan
Driver giao tiếp trực tiếp với cảm biến IMU MPU6050 qua chuẩn I2C (địa chỉ `MPU_ADDR = 0x68`, tốc độ bus `MPU_FREQ = 400 kHz`). Chịu trách nhiệm thiết lập các thanh ghi, định cấu hình dải đo gia tốc kế/con quay hồi chuyển (FS range), bộ lọc số DLPF, tần số lấy mẫu, quản lý năng lượng, ngắt (interrupt) và bộ đệm FIFO phần cứng.

Khác với mô tả cũ, driver **không** chỉ trả về byte/int16 thô: nó cung cấp **cả hai mức dữ liệu** — dữ liệu raw (`int16_t`, đơn vị LSB) và dữ liệu đã được scale sang **đơn vị vật lý** (gia tốc theo `g`, vận tốc góc theo `deg/s`, nhiệt độ theo `°C`). Driver cũng tự động trừ bias (offset tĩnh) của con quay hồi chuyển sau khi hiệu chuẩn.

## Các Struct & Enum Chính

### Dữ liệu cảm biến
```c
// Dữ liệu thô (LSB), đọc trực tiếp từ thanh ghi
typedef struct {
    int16_t ax, ay, az;   // gia tốc 3 trục
    int16_t temp;         // nhiệt độ thô
    int16_t gx, gy, gz;   // con quay 3 trục (đã trừ offset trong read_raw/read_fifo)
} mpu6050_data_raw_t;

// Dữ liệu đã quy đổi sang đơn vị vật lý
typedef struct {
    float ax, ay, az;     // [g]
    float temp;           // [°C]
    float gx, gy, gz;     // [deg/s]
} mpu6050_data_t;
```

### Bảng hệ số quy đổi (Sensitivity Scale Factor)
Định nghĩa ở `mpu6050.h`, dùng để chia giá trị raw → đơn vị vật lý theo dải đo đang cấu hình:
```c
gyro_ssf[]  = { 250DPS:131.0, 500DPS:65.5, 1000DPS:32.8, 2000DPS:16.4 }  // LSB/(deg/s)
accel_ssf[] = { 2G:16384.0, 4G:8192.0, 8G:4096.0, 16G:2048.0 }           // LSB/g
```

### Cấu hình thiết bị
- `mpu6050_config_t`: struct tổng hợp — `accel_fs`, `gyro_fs`, `dlpf_cfg`, `sample_rate_hz` cùng các struct con `pwr_cfg`, `int_cfg`, `fifo_cfg`.
- `mpu6050_pwr_cfg_t`: quản lý năng lượng — `temp_disable`, `gyro_standby`, `accel_standby`.
- `mpu6050_int_cfg_t`: cấu hình ngắt — `fifo_overflow_en`, `data_ready_en`, `active_low`, `open_drain`, `latch_en`.
- `mpu6050_fifo_cfg_t`: cấu hình FIFO — `fifo_enable`, `temp_fifo_en`, `gyro_fifo_en`, `accel_fifo_en`.
- Các enum: `gyro_fs_t` (250/500/1000/2000 DPS), `accel_fs_t` (2/4/8/16 G), `mpu6050_dlpf_t` (260/184/94/44/21/10/5 Hz).

## Public API

- `void mpu6050_init(i2c_master_bus_handle_t bus_handle)`: Gắn thiết bị vào bus I2C đã có, ping `WHO_AM_I`, đánh thức cảm biến (chọn PLL Gyro X làm clock source).
- `void mpu6050_config(const mpu6050_config_t *cfg)`: Áp dụng toàn bộ cấu hình — FS range, DLPF, tần số lấy mẫu (tự tính `SMPLRT_DIV`), power management, ngắt và FIFO.
- `mpu6050_data_raw_t mpu6050_read_raw(void)`: Đọc một mẫu raw (accel + temp + gyro). Gyro đã được **trừ offset** đã hiệu chuẩn; accel/temp giữ nguyên LSB.
- `mpu6050_data_t mpu6050_read(void)`: Đọc một mẫu và quy đổi sẵn sang đơn vị vật lý (`g`, `deg/s`, `°C`).
- `void mpu6050_raw_to_float(const mpu6050_data_raw_t *raw, mpu6050_data_t *data)`: Quy đổi một mẫu raw có sẵn sang đơn vị vật lý (chia `accel_ssf`/`gyro_ssf`, nhiệt độ `= raw/340 + 36.53`).
- `esp_err_t mpu6050_read_fifo(mpu6050_data_raw_t *data_array, uint16_t *count)`: Đọc nhiều mẫu từ FIFO phần cứng (xem mục dưới).
- `void mpu6050_reset_fifo(void)`: Reset FIFO phần cứng (bật bit `FIFO_RESET` trong `USER_CTRL`).
- `uint16_t mpu6050_get_sample_rate(void)`: Lấy tần số lấy mẫu đang cấu hình (ví dụ 100 Hz).
- `void mpu6050_calibrate_gyro(void)`: Tự động hiệu chuẩn **bias** của con quay hồi chuyển (xem mục dưới).

## Hiệu chuẩn Gyro Bias — `mpu6050_calibrate_gyro()`

Đo và bù **bias tĩnh** của con quay hồi chuyển (đơn vị `deg/s`), **không phải** bù trừ góc. Thiết bị phải đứng yên khi gọi. Quy trình:

1. Reset offset về 0; tính delay theo `sample_rate_hz` thực tế để mỗi lần đọc là một mẫu mới.
2. **Warm-up**: bỏ qua 50 mẫu đầu cho cảm biến ổn định.
3. **Pass 1**: lấy 200 mẫu (cấp phát trên heap), tính `mean` 3 trục.
4. Tính `variance` để lập ngưỡng outlier theo **3-sigma** (so sánh bình phương để tránh `sqrt`; có sàn tối thiểu tránh ngưỡng = 0).
5. **Pass 2**: tính lại mean sau khi loại các mẫu vượt ngưỡng (mẫu bị rung/nhiễu).
6. **Fallback**: nếu hơn 50% số mẫu là outlier (thiết bị bị di chuyển), dùng mean đơn giản từ Pass 1.

Offset thu được (theo LSB và deg/s) được log lại và áp dụng trong `mpu6050_read_raw()` và `mpu6050_read_fifo()`.

## Đọc FIFO — `mpu6050_read_fifo()`

- Tham số `*count` vừa là **INPUT** (số packet tối đa muốn đọc, thường bằng kích thước mảng `data_array`) vừa là **OUTPUT** (số mẫu thực tế đã đọc được).
- `packet_size` được suy ra từ cấu hình FIFO đang bật (`accel` +6 byte, `gyro` +6 byte, `temp` +2 byte). Nếu không bật trục nào → trả `ESP_ERR_INVALID_STATE`.
- **An toàn dữ liệu** (không liên quan watchdog):
  - Phát hiện **overflow** khi `bytes_in_fifo == 1024` → tự gọi `mpu6050_reset_fifo()`, đặt `*count = 0`, trả `ESP_OK`.
  - Phát hiện **misalignment** khi `bytes_in_fifo` không chia hết cho `packet_size` → tự reset, `*count = 0`, trả `ESP_OK`.
- Đọc burst từ thanh ghi `FIFO_R_W`, phân giải vào `data_array` theo thứ tự accel → temp → gyro; gyro được **trừ offset** sau khi đọc (vì FIFO lưu dữ liệu thô).

## Ghi chú
- Hàm `mpu6050_init()` trong code nhận tham số `i2c_master_bus_handle_t bus_handle` (theo file `.c`); driver dùng IDF I2C master driver mới (`driver/i2c_master.h`).
- Mọi việc scale `g`/`deg/s` đã được driver xử lý sẵn — lớp Service không cần nhân lại hệ số.
