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
    └─▶ Phát sự kiện IMU_EVT_BATCH_READY ra Event Loop trung tâm
```

### 1.1 Tối ưu hóa năng lượng với PCNT (Hardware Offloading)
Để giảm thiểu tối đa việc MCU bị đánh thức (wake-up) liên tục từ chế độ ngủ (Light/Deep Sleep) mỗi khi có 1 mẫu IMU mới (tương đương 100 lần/giây), hệ thống áp dụng kỹ thuật **Offloading bằng bộ đếm xung phần cứng PCNT (Pulse Counter)** của ESP32-S3:
* Thay vì kích hoạt ngắt phần mềm GPIO ISR cho từng xung tín hiệu `Data Ready` từ MPU6050, chân INT được dẫn trực tiếp vào module PCNT của ESP32-S3.
* **PCNT** sẽ tự động đếm các xung ngắt này ở mức phần cứng mà không cần bất kỳ sự can thiệp nào của CPU (CPU có thể ngủ hoàn toàn).
* CPU chỉ bị đánh thức (tạo ngắt) khi PCNT đếm đủ số lượng mẫu quy định (ví dụ: `IMU_BATCH_SIZE = 50` xung). Khi ngắt PCNT xảy ra, `imu_processing_task` mới được thông báo để thức dậy và tiến hành Burst Read một mạch toàn bộ dữ liệu từ FIFO của MPU6050.
* **Lợi ích cốt lõi**: Giảm đến 98% số lần đánh thức CPU và context switch, giúp tăng đáng kể thời lượng pin cho thiết bị đeo (wearable) trong khi vẫn đảm bảo thu thập đầy đủ 100% dữ liệu cảm biến.


---

## 2. KIẾN TRÚC CỬA SỔ TRƯỢT (SLIDING WINDOW) & BATCHING
*   **Cửa sổ trượt (Sliding Window)**: Có kích thước cố định `IMU_WINDOW_SIZE = 100` mẫu (tương đương 1 giây dữ liệu lịch sử chuyển động ở tần số 100Hz). Cửa sổ này liên tục cập nhật mẫu mới nhất và đẩy mẫu cũ nhất ra ngoài qua cấu trúc bộ đệm vòng (Circular Buffer). Đây chính là dữ liệu đầu vào cốt lõi để mô hình TinyML phân tích cử động trong Phase 2.
*   **Batching Mode**: Thay vì xử lý và truyền gói tin MQTT từng mẫu đơn lẻ (100 lần mỗi giây gây quá tải đường truyền), dịch vụ gom đủ `IMU_BATCH_SIZE = 50` mẫu (0.5 giây dữ liệu) rồi mới phát tín hiệu đóng gói gửi đi. Điều này giảm thiểu tối đa năng lượng tiêu thụ phát sóng WiFi/4G.

```c
typedef struct {
    float roll[IMU_WINDOW_SIZE];   // Mảng lưu góc roll lọc qua Kalman
    float pitch[IMU_WINDOW_SIZE];  // Mảng lưu góc pitch lọc qua Kalman
    uint16_t head;                 // Con trỏ đầu ghi của bộ đệm vòng
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

### 3.2 Lấy dữ liệu góc tức thời
```c
void imu_service_get_latest_angles(float *roll, float *pitch);
```
Đọc nhanh giá trị Roll và Pitch sau khi lọc Kalman tại thời điểm hiện tại. An toàn khi gọi từ các task khác nhờ cơ chế bảo vệ mutex/volatile nội bộ.

---

## 4. VÍ DỤ TÍCH HỢP HỆ THỐNG

### 4.1 Khởi tạo trong app_main.c
```c
#include "imu_service.h"

void app_main(void) {
    // Khởi tạo NVS, Bus I2C trước...
    
    // Khởi tạo dịch vụ IMU với ngắt tại chân GPIO 11
    esp_err_t err = imu_service_init(GPIO_NUM_11);
    if (err == ESP_OK) {
        ESP_LOGI("MAIN", "IMU Service initialized successfully!");
    }
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
