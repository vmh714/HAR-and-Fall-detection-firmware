# 🔄 Component: svc_imu (IMU Processing Service)

> **Mục đích**: Tầng dịch vụ cấp cao quản lý luồng dữ liệu IMU. Chịu trách nhiệm khởi tạo luồng ngắt FreeRTOS, đọc dữ liệu gom nhóm (FIFO Burst Read), chạy bộ lọc Kalman và duy trì cửa sổ trượt (Sliding Window).
> **Cập nhật cuối (Timestamp)**: 2026-05-21T15:07:00+07:00
> **Trạng thái**: Hoạt động ổn định

---

## 1. PHƯƠNG THỨC XỬ LÝ SỰ KIỆN & LUỒNG DỮ LIỆU BẤT ĐỒNG BỘ
Để đảm bảo tiết kiệm năng lượng và không bỏ sót bất kỳ mẫu dữ liệu nào tại tần số cao **100Hz**, dịch vụ sử dụng cơ chế **Event-driven** dựa trên ngắt phần cứng thay vì polling liên tục:

```
[ MPU6050 ]
    │ (Có mẫu dữ liệu mới / FIFO đầy)
    ▼ (Kéo chân INT GPIO 11 xuống thấp)
[ PNCT ]
    | Đếm đủ 50 xung ngắt mới gọi Gọi vTaskNotifyGiveFromISR()
    |
    │ ───▶ Gọi vTaskNotifyGiveFromISR()
    ▼
[ imu_processing_task ] (Thức dậy từ trạng thái Blocked, Priority 10)
    │
    ├─▶ Đọc cụm FIFO (Burst Read 50 mẫu) ➔ Tiết kiệm giao tiếp bus I2C
    ├─▶ Ánh xạ hệ trục tọa độ FLU (Body Frame)
    ├─▶ Tính góc nghiêng Roll/Pitch và đẩy qua bộ lọc Kalman 1D
    ├─▶ Cập nhật vào Bộ đệm vòng Cửa sổ trượt (Sliding Window 100 mẫu)
    └─▶ Gọi Callback đăng ký (được nạp từ app_main.c) ➔ Gửi batch thô sang Cloud Queue
```

### 1.1 Tối ưu hóa năng lượng với PCNT (Hardware Offloading)
Để giảm thiểu tối đa việc MCU bị đánh thức (wake-up) liên tục từ chế độ ngủ (Light/Deep Sleep) mỗi khi có 1 mẫu IMU mới (tương đương 100 lần/giây), hệ thống áp dụng kỹ thuật **Offloading bằng bộ đếm xung phần cứng PCNT (Pulse Counter)** của ESP32-S3:
* Thay vì kích hoạt ngắt phần mềm GPIO ISR cho từng xung tín hiệu `Data Ready` từ MPU6050, chân INT được dẫn trực tiếp vào module PCNT của ESP32-S3.
* **PCNT** sẽ tự động đếm các xung ngắt này ở mức phần cứng mà không cần bất kỳ sự can thiệp nào của CPU (CPU có thể ngủ hoàn toàn).
* CPU chỉ bị đánh thức (tạo ngắt) khi PCNT đếm đủ số lượng mẫu quy định (ví dụ: `IMU_BATCH_SIZE = 50` xung). Khi ngắt PCNT xảy ra, `imu_processing_task` mới được thông báo để thức dậy và tiến hành Burst Read một mạch toàn bộ dữ liệu từ FIFO của MPU6050.
* **Lợi ích cốt lõi**: Giảm đến 98% số lần đánh thức CPU và context switch, giúp tăng đáng kể thời lượng pin cho thiết bị đeo (wearable) trong khi vẫn đảm bảo thu thập đầy đủ 100% dữ liệu cảm biến.

### 1.2 Cơ chế Watchdog ngắt & Tự động phục hồi cảm biến (Self-Healing)
Trong thực tế, khi cảm biến MPU6050 hoạt động ở tần suất cao, bộ đệm FIFO có thể bị tràn hoặc ngắt tín hiệu vật lý INT bị treo mức logic (mất cạnh sườn), làm PCNT ngừng đếm và CPU bị khóa luồng IMU vĩnh viễn. Để khắc phục:
* Thay vì đợi ngắt vô hạn (`portMAX_DELAY`), task IMU sử dụng cấu hình chờ tối đa **1 giây (1000ms)**.
* Khi phát hiện quá thời gian (timeout) mà không nhận được thông báo từ PCNT, hệ thống sẽ tự động kích hoạt tiến trình giải phóng cảm biến: gọi `mpu6050_reset_fifo()` để xóa bộ đệm của MPU6050 và `pcnt_unit_clear_count()` để làm sạch bộ đếm PCNT.
* Tiến trình tự chữa lành (Self-Healing) này giúp firmware tự giải phóng và tiếp tục hoạt động trơn tru mà không cần nút nhấn reset cứng.


---

## 2. KIẾN TRÚC CỬA SỔ TRƯỢT (SLIDING WINDOW) & BATCHING
*   **Cửa sổ trượt (Sliding Window)**: Có kích thước cố định `IMU_WINDOW_SIZE = 100` mẫu (tương đương 1 giây dữ liệu lịch sử chuyển động ở tần số 100Hz). Cửa sổ này liên tục cập nhật mẫu mới nhất và đẩy mẫu cũ nhất ra ngoài qua cấu trúc bộ đệm vòng (Circular Buffer). Đây chính là dữ liệu đầu vào cốt lõi để mô hình TinyML phân tích cử động trong Phase 2.
*   **Batching Mode**: Thay vì xử lý và truyền gói tin MQTT từng mẫu đơn lẻ (100 lần mỗi giây gây quá tải đường truyền), dịch vụ gom đủ `IMU_BATCH_SIZE = 50` mẫu (0.5 giây dữ liệu) rồi mới phát tín hiệu đóng gói gửi đi. Điều này giảm thiểu tối đa năng lượng tiêu thụ phát sóng WiFi/4G.

```c
typedef struct {
    float ax[IMU_WINDOW_SIZE];
    float ay[IMU_WINDOW_SIZE];
    float az[IMU_WINDOW_SIZE];
    float gx[IMU_WINDOW_SIZE];
    float gy[IMU_WINDOW_SIZE];
    float gz[IMU_WINDOW_SIZE];
    uint16_t head;
} imu_window_t;
```

---

## 3. HƯỚNG DẪN SỬ DỤNG API
Các hàm công bố trong [imu_service.h](file:///d:/datn/firmware/components/svc_imu/include/imu_service.h):

### 3.1 Khởi tạo dịch vụ
```c
esp_err_t imu_service_init(gpio_num_t int_pin);
```
*   Khởi tạo driver `drv_mpu6050`, cấu hình chân ngắt GPIO vật lý nối với MPU6050.
*   Thiết lập hiệu chuẩn Gyro tĩnh.
*   Tạo Task điều phối FreeRTOS `imu_task` với mức độ ưu tiên cao (`Priority 10`) để xử lý thời gian thực nhanh nhất.

### 3.2 Lấy dữ liệu tư thế (Pitch) tức thời
```c
void imu_service_get_latest_pitch(float *pitch);
```
Đọc nhanh giá trị Pitch sau khi được dung hợp bởi thuật toán Kalman Fusion (2 trạng thái) tại thời điểm hiện tại. Góc này dùng độc lập để xác định tư thế người (đứng/nằm/ngồi). An toàn khi gọi từ task khác nhờ cơ chế bảo vệ nội bộ.

### 3.3 Đăng ký Callback xử lý Batch (Tránh Circular Dependency)
```c
typedef esp_err_t (*imu_batch_callback_t)(const void *batch_data);
void imu_service_register_batch_callback(imu_batch_callback_t cb);
```
* **Mô tả**: Đăng ký một con trỏ hàm callback (nhận gói dữ liệu IMU batch) để chuyển dữ liệu trực tiếp sang tầng Cloud Queue.
* **Tầm quan trọng**: Giúp `svc_imu` hoàn toàn decoupled khỏi `svc_cloud` (không cần include header chéo), giải quyết triệt để vấn đề phụ thuộc vòng tròn trong hệ thống build CMake của ESP-IDF.

---

## 4. VÍ DỤ TÍCH HỢP HỆ THỐNG

### 4.1 Khởi tạo và liên kết các dịch vụ trong app_main.c
```c
#include "imu_service.h"
#include "svc_cloud.h"

void app_main(void) {
    // 1. Khởi tạo NVS, Bus I2C, MPU6050 trước...
    
    // 2. Khởi tạo dịch vụ Cloud (hàng đợi nhận batch IMU nằm trong này)
    svc_cloud_init(CONFIG_MQTT_BROKER_URI, CONFIG_DEVICE_ID, CONFIG_MQTT_USERNAME, CONFIG_MQTT_PASSWORD);

    // 3. Khởi tạo dịch vụ IMU với ngắt tại chân GPIO 11
    imu_service_init(GPIO_NUM_11);

    // 4. Đăng ký liên kết luồng dữ liệu (Callback đẩy thẳng dữ liệu vào Cloud Queue)
    imu_service_register_batch_callback(svc_cloud_enqueue_imu_batch);
}
```

### 4.2 Lấy dữ liệu góc từ một Task hiển thị/điều khiển khác
```c
void display_task(void *pvParameters) {
    float r, p;
    while(1) {
        imu_service_get_latest_angles(&r, &p);
        ESP_LOGI("DISPLAY", "Current Roll: %.2f, Pitch: %.2f", r, p);
        vTaskDelay(pdMS_TO_TICKS(500)); // Cập nhật màn hình mỗi 500ms
    }
}
```
---
