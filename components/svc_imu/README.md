# 🔄 Component: svc_imu (IMU Processing Service)

> **Mục đích**: Tầng dịch vụ cấp cao quản lý luồng dữ liệu IMU. Chịu trách nhiệm khởi tạo luồng ngắt FreeRTOS, đọc dữ liệu gom nhóm (FIFO Burst Read), chạy bộ lọc Kalman và duy trì cửa sổ trượt (Sliding Window).
> **Cập nhật cuối (Timestamp)**: 2026-06-30
> **Trạng thái**: Hoạt động ổn định

---

## 1. PUBLIC API
Các hàm công bố trong [imu_service.h](include/imu_service.h):

### 1.1 Khởi tạo dịch vụ
```c
esp_err_t imu_service_init(gpio_num_t int_pin);
```
*   Khởi tạo driver `drv_mpu6050`, cấu hình chân ngắt GPIO vật lý nối với MPU6050.
*   Thiết lập hiệu chuẩn Gyro tĩnh.
*   **Hot Start**: Sau khi calibrate, service đọc một mẫu thực rồi seed thẳng giá trị ban đầu cho cả Kalman Roll (`kal_roll.angle`) lẫn 6 bộ Kalman 1D (`kf_ax..kf_gz`), tránh hiện tượng ramp-up từ 0 trong các giây đầu.
*   Tạo Task điều phối FreeRTOS `imu_task` với mức độ ưu tiên cao (`Priority 10`) để xử lý thời gian thực nhanh nhất.

### 1.2 Lấy dữ liệu tư thế (Roll) tức thời
```c
void imu_service_get_latest_roll(float *roll);
```
Đọc nhanh giá trị Roll sau khi được dung hợp bởi thuật toán Kalman Fusion (2 trạng thái) tại thời điểm hiện tại. Góc này dùng độc lập để xác định tư thế người (đứng/nằm/ngồi). An toàn khi gọi từ task khác nhờ cơ chế bảo vệ nội bộ.

### 1.3 Đăng ký Callback xử lý Batch (Tránh Circular Dependency)
```c
typedef esp_err_t (*imu_batch_callback_t)(const void *batch_data);
void imu_service_register_batch_callback(imu_batch_callback_t cb);
```
* **Mô tả**: Đăng ký một con trỏ hàm callback (nhận gói dữ liệu IMU batch) để chuyển dữ liệu trực tiếp sang tầng Cloud Queue.
* **Tầm quan trọng**: Giúp `svc_imu` hoàn toàn decoupled khỏi `svc_cloud` (không cần include header chéo), giải quyết triệt để vấn đề phụ thuộc vòng tròn trong hệ thống build CMake của ESP-IDF.

---

## 2. VÍ DỤ TÍCH HỢP HỆ THỐNG

### 2.1 Khởi tạo và liên kết các dịch vụ trong app_main.c
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

### 2.2 Lấy dữ liệu góc từ một Task hiển thị/điều khiển khác
```c
void display_task(void *pvParameters) {
    float r;
    while(1) {
        imu_service_get_latest_roll(&r);
        ESP_LOGI("DISPLAY", "Current Roll: %.2f", r);
        vTaskDelay(pdMS_TO_TICKS(500)); // Cập nhật màn hình mỗi 500ms
    }
}
```

---

## 3. CƠ CHẾ HOẠT ĐỘNG: XỬ LÝ SỰ KIỆN & LUỒNG DỮ LIỆU BẤT ĐỒNG BỘ
Để đảm bảo tiết kiệm năng lượng và không bỏ sót bất kỳ mẫu dữ liệu nào tại tần số cao **100Hz**, dịch vụ sử dụng cơ chế **Event-driven** dựa trên ngắt phần cứng thay vì polling liên tục:

```text
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
    ├─▶ Tính góc Roll và dung hợp bằng Kalman 2-trạng thái (kal_roll) ➔ Xác định tư thế (nằm/đứng/ngồi) (D-013)
    ├─▶ Lọc Kalman 1D cho CẢ 6 TRỤC (kf_ax..kf_gz) ➔ Khử nhiễu gia tốc kế / con quay
    ├─▶ Chuẩn hóa 6 trục về thang [-1, 1] theo full-scale range ➔ Làm input cho TinyML INT8
    ├─▶ Cập nhật vào Bộ đệm vòng Cửa sổ trượt (Sliding Window 200 mẫu)
    ├─▶ Mỗi lô 50 mẫu gọi svc_ai_process_window(&imu_win) ➔ Đẩy window sang svc_ai inference (chỉ chạy ở STATE_NORMAL)
    └─▶ (Chỉ STATE_STREAMING) Gọi Callback đăng ký ➔ Gửi batch (đã đổi trục + lọc Kalman + scale int16) sang Cloud Queue
```

### 3.1 Tối ưu hóa năng lượng với PCNT (Hardware Offloading)
Để giảm thiểu tối đa việc MCU bị đánh thức (wake-up) liên tục từ chế độ ngủ (Light/Deep Sleep) mỗi khi có 1 mẫu IMU mới (tương đương 100 lần/giây), hệ thống áp dụng kỹ thuật **Offloading bằng bộ đếm xung phần cứng PCNT (Pulse Counter)** của ESP32-S3:
* Thay vì kích hoạt ngắt phần mềm GPIO ISR cho từng xung tín hiệu `Data Ready` từ MPU6050, chân INT được dẫn trực tiếp vào module PCNT của ESP32-S3.
* **PCNT** sẽ tự động đếm các xung ngắt này ở mức phần cứng mà không cần bất kỳ sự can thiệp nào của CPU (CPU có thể ngủ hoàn toàn).
* CPU chỉ bị đánh thức (tạo ngắt) khi PCNT đếm đủ số lượng mẫu quy định (ví dụ: `IMU_BATCH_SIZE = 50` xung). Khi ngắt PCNT xảy ra, `imu_processing_task` mới được thông báo để thức dậy và tiến hành Burst Read một mạch toàn bộ dữ liệu từ FIFO của MPU6050.
* **Lợi ích cốt lõi**: Giảm đến 98% số lần đánh thức CPU và context switch, giúp tăng đáng kể thời lượng pin cho thiết bị đeo (wearable) trong khi vẫn đảm bảo thu thập đầy đủ 100% dữ liệu cảm biến.

### 3.2 Cơ chế Watchdog ngắt & Tự động phục hồi cảm biến (Self-Healing)
Trong thực tế, khi cảm biến MPU6050 hoạt động ở tần suất cao, bộ đệm FIFO có thể bị tràn hoặc ngắt tín hiệu vật lý INT bị treo mức logic (mất cạnh sườn), làm PCNT ngừng đếm và CPU bị khóa luồng IMU vĩnh viễn. Để khắc phục:
* Thay vì đợi ngắt vô hạn (`portMAX_DELAY`), task IMU sử dụng cấu hình chờ tối đa **1 giây (1000ms)**.
* Khi phát hiện quá thời gian (timeout) mà không nhận được thông báo từ PCNT, hệ thống sẽ tự động kích hoạt tiến trình giải phóng cảm biến: gọi `mpu6050_reset_fifo()` để xóa bộ đệm của MPU6050 và `pcnt_unit_clear_count()` để làm sạch bộ đếm PCNT.
* Tiến trình tự chữa lành (Self-Healing) này giúp firmware tự giải phóng và tiếp tục hoạt động trơn tru mà không cần nút nhấn reset cứng.

---

## 4. KIẾN TRÚC CỬA SỔ TRƯỢT (SLIDING WINDOW) & BATCHING
*   **Cửa sổ trượt (Sliding Window)**: Có kích thước cố định `IMU_WINDOW_SIZE = 200` mẫu (tương đương 2 giây dữ liệu lịch sử chuyển động ở tần số 100Hz). Cửa sổ này liên tục cập nhật mẫu mới nhất và đẩy mẫu cũ nhất ra ngoài qua cấu trúc bộ đệm vòng (Circular Buffer). Toàn bộ 6 trục lưu trong cửa sổ đã được **chuẩn hóa về thang (-1, 1)** để làm đầu vào trực tiếp cho mô hình TinyML đã quantize INT8.
*   **Batching Mode (chỉ ở STATE_STREAMING)**: Thay vì xử lý và truyền gói tin MQTT từng mẫu đơn lẻ (100 lần mỗi giây gây quá tải đường truyền), dịch vụ gom đủ `IMU_BATCH_SIZE = 50` mẫu (0.5 giây dữ liệu) rồi mới phát tín hiệu đóng gói gửi đi. Batch chỉ được gom khi hệ thống ở `STATE_STREAMING`; dữ liệu trong batch **không phải dữ liệu thô** mà đã được đổi hệ trục (Body frame), lọc Kalman 1D và scale lại về `int16_t` để đồng nhất với dữ liệu inference của TinyML. Điều này giảm thiểu tối đa năng lượng tiêu thụ phát sóng WiFi/4G.

```c
// Sliding window chứa 6 trục IMU đã chuẩn hóa về (-1, 1) cho TinyML INT8
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

### 4.1 Ma trận hoạt động theo FSM State
Để tối ưu CPU và tránh các cảnh báo ảo/hành vi sai thiết kế, các chức năng của IMU Service được phân luồng (gating) nghiêm ngặt dựa trên state của hệ thống:

| Hoạt động | INIT | CONNECTING | NORMAL | STREAMING | OTA |
|---|---|---|---|---|---|
| Drain FIFO (luôn đọc, chống tràn FIFO) | ✓ | ✓ | ✓ | ✓ | ✓ |
| Kalman ×7 + normalize `imu_win` | – | – | ✓ | ✓ | – |
| Impact/free-fall + pedometer + roll | – | – | ✓ | – | – |
| svc_ai inference (HAR + fall) | – | – | ✓ | – | – |
| Fill batch + publish `imu_stream` | – | – | – | ✓ | – |
| Telemetry `status` | – | – | ✓ | – | – |
| Alert `alert/fall` (event-driven) | – | – | ✓ | – | – |
| RSSI probe `+++`/ATO | – | – | ✓ | – | – |

*Lưu ý:* Ở `STATE_STREAMING`, hệ thống **bỏ qua hoàn toàn** việc xử lý AI (inference), đếm bước (pedometer), và phát hiện va chạm (impact), chỉ tập trung lọc Kalman và gom batch để tối đa hóa tài nguyên cho quá trình thu thập dataset.

Bên cạnh `imu_window_t` (đầu vào cho inference), service còn định nghĩa hai kiểu dữ liệu phục vụ luồng streaming qua callback:

```c
// Một mẫu IMU 6 trục đã đổi trục + lọc Kalman + scale lại int16
typedef struct __attribute__((packed)) {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
} imu_stream_data_t;

// Gói batch chứa tối đa IMU_BATCH_SIZE mẫu, là kiểu dữ liệu callback nhận được
typedef struct {
    imu_stream_data_t data[IMU_BATCH_SIZE];
    uint16_t count;
} imu_batch_data_t;
```
