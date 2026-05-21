# 🔌 Component: drv_mpu6050 (MPU6050 IMU Driver)

> **Mục đích**: Lớp driver giao tiếp phần cứng trực tiếp với cảm biến gia tốc và góc quay MPU6050 thông qua giao thức I2C.
> **Cập nhật cuối (Timestamp)**: 2026-05-21T15:07:00+07:00
> **Trạng thái**: Hoạt động ổn định (Duy trì cho Phase 1 và Phase 2)

---

## 1. NGUYÊN LÝ HOẠT ĐỘNG & THIẾT LẬP PHẦN CỨNG
Cảm biến MPU6050 đo lường gia tốc 3 trục (Accelerometer) và vận tốc góc 3 trục (Gyroscope). Để phục vụ tối ưu cho bài toán nhận dạng hoạt động (HAR) và phát hiện té ngã, driver được cấu hình mặc định như sau:

| Thông số | Giá trị cấu hình | Ý nghĩa kỹ thuật |
| :--- | :--- | :--- |
| **Giao tiếp** | I2C Master @ 400 kHz | Tốc độ cao để giảm tối đa độ trễ đọc bus. |
| **Dải đo Accel** | `ACCEL_FS_8G` (±8g) | Tránh hiện tượng bão hòa tín hiệu khi va đập mạnh xảy ra lúc ngã (cú va đập có thể lên tới > 4g). |
| **Dải đo Gyro** | `GYRO_FS_500DPS` (±500°/s) | Cân bằng hoàn hảo giữa độ nhạy đo lường và phạm vi quay của thắt lưng. |
| **Lọc phần cứng** | `DLPF_CFG_21HZ` (LBF ~21Hz) | Loại bỏ nhiễu rung tần số cao từ môi trường ngoài. |
| **Tần số lấy mẫu** | **100 Hz** (10ms một mẫu) | Đảm bảo thu thập đầy đủ chi tiết chuyển động mà không quá tải MCU. |
| **Ngắt cơ bản** | `Data Ready` (Active Low) | Chân INT (GPIO 11) kéo xuống thấp báo hiệu có mẫu mới ➔ Đọc dữ liệu tự động, loại bỏ polling. |
| **Bộ đệm** | Hardware FIFO | Đẩy Accel và Gyro vào FIFO ➔ Chống mất mẫu khi CPU bận. |

---

## 2. HỆ TỌA ĐỘ VÀ AXIS MAPPING (FLU CONVENTION)
Thiết bị được thiết kế để đeo ở **thắt lưng phía trước**. Khi đeo đúng tư thế, hệ tọa độ từ cảm biến phải được mapping sang hệ tọa độ cơ thể (Body Frame) như sau để thuật toán ML nhận diện chính xác:

```
                  ^ Y_body (Lên trời, ~1g tĩnh)
                  |
                  |     / Z_body (Hướng tiến về trước)
                  |    /
                  |   /
   X_body <-------o──/
 (Sang trái)
```

**Mã nguồn ánh xạ trong Driver:**
```c
float ax_body = processed_data.az;   // X_body = sensor_az
float ay_body = processed_data.ax;   // Y_body = sensor_ax (1g khi đứng yên)
float az_body = processed_data.ay;   // Z_body = sensor_ay

float gx_body = processed_data.gz;   // Roll rate
float gy_body = processed_data.gx;   // Pitch rate
```
> ⚠️ **QUY TẮC CỨNG**: Không được thay đổi Axis Mapping này trừ khi có yêu cầu thay đổi vị trí đeo thiết bị vật lý. Sai lệch trục sẽ phá hỏng toàn bộ mô hình TinyML.

---

## 3. THUẬT TOÁN HIỆU CHUẨN GYRO (GYRO CALIBRATION)
Cảm biến IMU luôn tồn tại sai số lệch không (Zero-rate offset) của Gyro do nhiệt độ và chế tạo sai lệch. Driver tích hợp thuật toán hiệu chuẩn tĩnh khi khởi động (Calibration):
1.  **Thu thập 200 mẫu liên tục** (bội số của Batch size 50, mất ~2.75 giây khi boot).
2.  **Yêu cầu**: Thiết bị bắt buộc phải đặt đứng yên hoàn toàn trong thời gian này.
3.  **Thuật toán lọc**: Tính giá trị trung bình cộng (Mean Offset) của 3 trục góc quay để tìm sai số tĩnh.
4.  **Kết quả**: Sai số sau hiệu chuẩn đạt độ chính xác nhiễu `σ_mean ≈ 0.0016 °/s`, nhỏ hơn nhiều so với độ phân giải nhạy của cảm biến, đảm bảo bộ lọc góc nghiêng không bị trôi (Drift).

---

## 4. HƯỚNG DẪN SỬ DỤNG API
Các hàm công bố trong [mpu6050.h](file:///d:/datn/firmware/components/drv_mpu6050/include/mpu6050.h):

### 4.1 Khởi tạo và Cấu hình
```c
void mpu6050_init(void);
void mpu6050_config(const mpu6050_config_t *cfg);
```
Khởi tạo phần cứng bus I2C vật lý và thiết lập dải đo, ngắt, bộ lọc DLPF dựa trên struct cấu hình.

### 4.2 Hiệu chuẩn Gyroscope
```c
void mpu6050_calibrate_gyro(void);
```
Bắt đầu đo 200 mẫu tĩnh, tính toán Offset Gyro và lưu trữ vào biến toàn cục tĩnh trong driver để tự động trừ đi trong các lần đọc tiếp theo.

### 4.3 Đọc dữ liệu trực tiếp
```c
mpu6050_data_raw_t mpu6050_read_raw(void);
mpu6050_data_t mpu6050_read(void);
```
*   `mpu6050_read_raw`: Đọc trực tiếp các giá trị số nguyên 16-bit thô từ thanh ghi cảm biến.
*   `mpu6050_read`: Đọc dữ liệu thô, tự động áp dụng hệ số chuyển đổi nhạy (Scale Factors) và tính toán ra đơn vị vật lý thực tế (`g` và `°/s`).

### 4.4 Quản lý Bộ đệm FIFO phần cứng
```c
void mpu6050_reset_fifo(void);
esp_err_t mpu6050_read_fifo(mpu6050_data_raw_t *data_array, uint16_t *count);
```
*   `mpu6050_reset_fifo`: Xóa sạch bộ đệm FIFO vật lý trên chip để tránh đọc phải dữ liệu rác cũ tích lũy khi CPU bị gián đoạn.
*   `mpu6050_read_fifo`: Đọc theo khối (Batch) toàn bộ các mẫu dữ liệu thô tích lũy trong bộ đệm FIFO sang mảng vùng nhớ tạm RAM trên ESP32. Rất quan trọng để tối ưu hóa việc truyền dữ liệu tốc độ cao không mất mẫu.
```c
// Ví dụ đọc FIFO
mpu6050_data_raw_t fifo_buf[100];
uint16_t samples_read = 0;
esp_err_t err = mpu6050_read_fifo(fifo_buf, &samples_read);
```
---
