# Component: drv_mpu6050

> **Cập nhật cuối (Timestamp):** 2026-06-30
> **Trạng thái:** Hoạt động ổn định

---

## 1. PUBLIC API & CẤU TRÚC DỮ LIỆU

### 1.1 Các hàm khởi tạo và cấu hình
- `void mpu6050_init(i2c_master_bus_handle_t bus_handle)`: Gắn thiết bị vào bus I2C đã có, ping `WHO_AM_I`, đánh thức cảm biến (chọn PLL Gyro X làm clock source).
- `void mpu6050_config(const mpu6050_config_t *cfg)`: Áp dụng toàn bộ cấu hình — FS range, DLPF, tần số lấy mẫu (tự tính `SMPLRT_DIV`), power management, ngắt và FIFO.
- `void mpu6050_calibrate_gyro(void)`: Tự động hiệu chuẩn **bias** của con quay hồi chuyển. Thiết bị phải đứng yên khi gọi.
- `uint16_t mpu6050_get_sample_rate(void)`: Lấy tần số lấy mẫu đang cấu hình.

### 1.2 Đọc dữ liệu
- `mpu6050_data_raw_t mpu6050_read_raw(void)`: Đọc một mẫu raw (accel + temp + gyro). Gyro đã được **trừ offset** đã hiệu chuẩn; accel/temp giữ nguyên LSB.
- `mpu6050_data_t mpu6050_read(void)`: Đọc một mẫu và quy đổi sẵn sang đơn vị vật lý (`g`, `deg/s`, `°C`).
- `void mpu6050_raw_to_float(const mpu6050_data_raw_t *raw, mpu6050_data_t *data)`: Quy đổi một mẫu raw có sẵn sang đơn vị vật lý (chia hệ số scale).
- `esp_err_t mpu6050_read_fifo(mpu6050_data_raw_t *data_array, uint16_t *count)`: Đọc nhiều mẫu (Burst Read) từ FIFO phần cứng.
- `void mpu6050_reset_fifo(void)`: Reset FIFO phần cứng.

### 1.3 Cấu trúc dữ liệu chính
```c
// Dữ liệu thô (LSB), đọc trực tiếp từ thanh ghi (gyro đã được driver trừ offset)
typedef struct { int16_t ax, ay, az, temp, gx, gy, gz; } mpu6050_data_raw_t;

// Dữ liệu đã quy đổi sang đơn vị vật lý (g, °C, deg/s)
typedef struct { float ax, ay, az, temp, gx, gy, gz; } mpu6050_data_t;
```

---

## 2. Mục đích (Purpose / Why)
Driver `drv_mpu6050` là tầng giao tiếp trực tiếp với cảm biến phần cứng qua chuẩn I2C. Nó giấu đi toàn bộ sự phức tạp của việc ghi/đọc thanh ghi, định cấu hình dải đo (FS range), bộ lọc số (DLPF), cấu hình ngắt phần cứng, và đặc biệt là xử lý bộ đệm FIFO của cảm biến. Tầng dịch vụ (`svc_imu`) phía trên chỉ việc gọi các hàm cấp cao để nhận dữ liệu nguyên bản hoặc dữ liệu vật lý một cách dễ dàng.

## 3. Cơ chế cốt lõi (How it works)

### 3.1 Quy đổi đơn vị (Scale Factor)
Khác với các driver sơ khai, driver này không chỉ trả về LSB thô. Dựa vào cấu hình FS range (VD: Gyro `500DPS` → chia `65.5`, Accel `4G` → chia `8192.0`), nó tự động thực hiện việc quy đổi nội bộ thông qua hàm `mpu6050_raw_to_float()`. Điều này giải phóng lớp Service khỏi việc phải nhân/chia thủ công hệ số.

### 3.2 Hiệu chuẩn Gyro Bias (3-Sigma Filter)
Để giải quyết hiện tượng trôi (drift) của con quay hồi chuyển, hàm `mpu6050_calibrate_gyro()` thực hiện đo và bù **bias tĩnh**:
1. Bỏ qua 50 mẫu đầu (Warm-up).
2. Lấy 200 mẫu, tính trung bình (mean) và phương sai (variance).
3. Sử dụng bộ lọc **3-Sigma**: loại bỏ các mẫu (outlier) do vô tình rung lắc thiết bị trong lúc hiệu chuẩn.
4. Tính lại trung bình lần 2 trên các mẫu sạch.
5. Fallback: Nếu rung lắc quá mạnh (>50% outlier), dùng luôn giá trị mean ban đầu.

### 3.3 Đọc Burst FIFO và Phục hồi lỗi (Misalignment)
- **Burst Read:** Đọc toàn bộ packet (accel + temp + gyro) trong 1 lần I2C transaction từ thanh ghi `FIFO_R_W`, giảm đáng kể overhead bus.
- **Tự động phục hồi:**
  - *Overflow:* Nếu FIFO đầy (`bytes_in_fifo == 1024`), driver tự gọi hàm `mpu6050_reset_fifo()` để xóa bộ đệm, trả về 0 mẫu nhưng không văng lỗi sập chương trình.
  - *Misalignment:* Nếu số byte đọc được không chia hết cho kích thước packet (bị mất đồng bộ), driver cũng chủ động reset FIFO để cứu vãn.

## 4. Đa nhiệm & Tài nguyên (Concurrency & Resources)
* Driver thuần cấp thấp, **không sinh Task**.
* Việc cấp phát vùng nhớ 200 mẫu khi hiệu chuẩn Gyro diễn ra trên Heap (`malloc`), nhưng được giải phóng ngay sau đó (`free`) để tiết kiệm RAM tĩnh (BSS).
* Truy cập I2C an toàn đa nhiệm nhờ tính năng Thread-safe có sẵn của I2C Master Driver trong ESP-IDF v5+.

## 5. Luồng giao tiếp (Data Flow)
* **Khởi tạo:** `svc_imu` truyền I2C bus handle (được tạo ở `app_main`) xuống để driver gắn vào bus.
* **Ngắt Data Ready:** Driver này cấu hình MPU6050 để xuất tín hiệu LOW tại chân INT vật lý mỗi khi FIFO đầy hoặc có mẫu mới. Việc nhận tín hiệu INT vật lý này thuộc trách nhiệm của `svc_imu` (qua PCNT).
* **Đọc dữ liệu:** `svc_imu` gọi `mpu6050_read_fifo()` để xả dữ liệu từ module này vào Sliding Window.
