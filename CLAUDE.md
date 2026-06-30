# Firmware — HAR & Post-Impact Fall Detection (ESP32-S3)

## Phần cứng mục tiêu
- **MCU**: Seeed Studio XIAO ESP32S3 — 8 MB PSRAM OPI 80 MHz + 8 MB Flash
- **IMU**: MPU6050 — I2C, lấy mẫu 100 Hz bằng PCNT hardware interrupt
- **4G LTE**: SIMCom A7680C — UART AT command + PPPoS (LwIP stack)
- **Build system**: ESP-IDF (CMake), partition tùy chỉnh ở `partitions.csv`

## Tech Stack
- **RTOS**: FreeRTOS (tích hợp sẵn trong ESP-IDF)
- **Event bus**: `esp_event` — toàn bộ giao tiếp giữa task qua Event Loop, không gọi trực tiếp
- **TinyML**: TensorFlow Lite Micro — kéo qua IDF Component Manager (`lib_tinyml/idf_component.yml`)
- **MQTT**: ESP-MQTT component (built-in IDF)
- **Ngôn ngữ**: C (`.c`/`.h`) cho toàn bộ logic; C++ (`.cpp`) chỉ ở `lib_tinyml/` do TFLite yêu cầu

## Kiến trúc phân lớp

```
Application : app_main  ──►  sys_manager (FSM + Event Loop trung tâm)
              ↓
Service     : svc_imu | svc_ai | svc_network | svc_cloud
              ↓
Library     : lib_kalman | lib_tinyml | lib_model
              ↓
Driver      : drv_mpu6050 | drv_a7680c
              ↓
Hardware    : MPU6050 (I2C) | A7680C (UART)
```

**Quy ước đặt tên prefix:**
- `drv_` — driver phần cứng, chỉ đọc/ghi thanh ghi, không có logic nghiệp vụ
- `svc_` — FreeRTOS Task, xử lý logic, quản lý event
- `lib_` — thuật toán thuần toán học, platform-independent
- `sys_` — điều phối toàn hệ thống (FSM, event base definitions)

## Cấu trúc thư mục

```
main/
├── app_main.c              # Entry point: khởi tạo và start tất cả services
├── hardware_config.h       # Pin map, I2C/UART config — sửa đây khi đổi phần cứng
├── esp32s3_mpu6050.c       # [LEGACY Phase 0] prototype monolithic — KHÔNG sửa
└── esp32s3_wifi_mqtt.c     # [LEGACY Phase 0] prototype monolithic — KHÔNG sửa

components/
├── drv_mpu6050/            # I2C read/write MPU6050, cấu hình FIFO, ngắt
├── drv_a7680c/             # AT command UART cho A7680C, GPIO power control
├── lib_kalman/             # Kalman filter 1D — lọc nhiễu gia tốc kế
├── lib_model/
│   └── model_data.cc       # Model TFLite quantized INT8 dưới dạng C byte array
├── lib_tinyml/
│   ├── tflite_wrapper.cpp  # Khởi tạo interpreter, chạy inference, trả kết quả
│   └── idf_component.yml   # Kéo TFLite Micro từ IDF Component Manager
├── svc_imu/                # Task 100Hz: đọc MPU6050 → Kalman → Sliding Window → event
├── svc_ai/                 # Task inference: nhận window từ svc_imu → lib_tinyml → alert event
├── svc_network/            # Task mạng: WiFi (dev) hoặc PPPoS/4G (production)
├── svc_cloud/              # Task MQTT: publish telemetry + alert, subscribe command
└── sys_manager/            # Định nghĩa FSM states, event base, hàm transition
```

## FSM — Các trạng thái chính

| State | Mô tả | Drain FIFO | Kalman/Normalize | AI / Pedometer | Publish Status | Publish Stream |
|---|---|:---:|:---:|:---:|:---:|:---:|
| `STATE_INIT` | Khởi tạo phần cứng | ✓ | – | – | – | – |
| `STATE_CONNECTING` | Chờ mạng, tự retry | ✓ | – | – | – | – |
| `STATE_NORMAL` | **Production**: AI inference liên tục | ✓ | ✓ | ✓ | ✓ | – |
| `STATE_STREAMING` | **Data collection**: raw IMU batch | ✓ | ✓ | – | – | ✓ |
| `STATE_OTA` | Nhận firmware → restart | ✓ | – | – | – | – |

*Luôn drain FIFO ở mọi state để chống tràn, nhưng chỉ thực hiện tính toán (Kalman, AI) ở NORMAL hoặc STREAMING.*

## Luồng dữ liệu chính (STATE_NORMAL)

```
MPU6050 (100Hz ngắt)
  └── drv_mpu6050 ──► svc_imu (Kalman filter + Sliding Window 200 mẫu, trượt 50)
                           └── IMU_EVT_WINDOW_READY ──► svc_ai
                                   └── lib_tinyml inference (32ms)
                                         ├── Bình thường → cập nhật trạng thái
                                         └── Fall Detected → AI_EVT_FALL_DETECTED
                                                   └── sys_manager ──► svc_cloud
                                                             └── MQTT publish alert (QoS 1)
```

*(Lưu ý: Ở `STATE_STREAMING`, luồng AI bị chặn lại để nhường toàn bộ CPU cho việc đóng gói và gửi IMU batch qua `svc_cloud`)*

## MQTT Topics

| Topic | Hướng | Mô tả |
|---|---|---|
| `eldercare/{id}/alert/fall` | Publish | Cảnh báo té ngã, QoS 1 — payload `{user_name, message, confidence}` |
| `eldercare/{id}/status` | Publish | Telemetry định kỳ (pin, bước chân, state, ai_pred, ...), QoS 0 — chỉ ở STATE_NORMAL |
| `eldercare/{id}/imu_stream` | Publish | IMU batch đã tiền xử lý (chỉ STATE_STREAMING), QoS 0 |
| `eldercare/{id}/command` | Subscribe | Nhận lệnh từ backend (start/stop stream, set_interval, OTA), QoS 1 |

## Quy ước quan trọng
- **Không gọi trực tiếp giữa các svc_**: mọi giao tiếp phải qua `esp_event_post()` → `sys_manager` xử lý
- **Sliding Window**: 200 mẫu (2s ở 100Hz), trượt 50 mẫu (0.5s) — không thay đổi mà không retrain model
- **Cooldown chống spam**: `svc_cloud` im lặng 15 giây cố định (`FALL_COOLDOWN_US`) sau mỗi lần gửi alert; `svc_ai` không có cooldown, phát event mỗi lần phát hiện ngã
- **Watchdog MQTT**: `svc_cloud` có watchdog tự phục hồi 2 bậc (stop/start client sau 60s, restart mạch sau 150s kẹt liên tục).
- **`lib_model/model_data.cc`**: file này được sinh tự động từ TFLite — KHÔNG sửa tay

## Quy ước comment (code style)

Áp dụng cho toàn bộ code firmware (`.c` / `.h` / `.cpp`), **trừ** file legacy Phase 0 (`main/esp32s3_*.c` — không sửa). Ba loại comment, dùng đúng ký hiệu:

- **Đầu hàm — BẮT BUỘC dùng block Doxygen `/** ... */`** đặt ngay trước hàm (ưu tiên hàm public trong header và hàm xử lý chính trong `.c`). Nêu ngắn gọn nhiệm vụ, kèm `@param` / `@return` khi cần:
  ```c
  /**
   * @brief Khởi tạo dịch vụ IMU: cấu hình MPU6050, bộ lọc Kalman, PCNT và tạo task xử lý.
   * @param int_pin Chân GPIO nối tới chân INT của MPU6050.
   * @return ESP_OK nếu khởi tạo thành công.
   */
  esp_err_t imu_service_init(gpio_num_t int_pin);
  ```
- **`///` — comment nguyên lý / quyết định thiết kế quan trọng** (vì sao làm vậy, ràng buộc phần cứng, mẹo kỹ thuật cốt lõi). Là phần người đọc cần để hiểu bản chất hệ thống:
  ```c
  /// PCNT đếm xung INT bằng phần cứng → CPU chỉ thức dậy mỗi 50 mẫu thay vì 100 lần/giây.
  ```
- **`//` — comment nội bộ / debug / làm rõ một bước** (giải thích tức thời cho một dòng lệnh):
  ```c
  // Phòng hờ sample_rate = 0 do lỗi đọc cấu hình
  if (sample_rate == 0) sample_rate = 100;
  ```

## Tài liệu tham khảo chi tiết
- Kiến trúc đầy đủ + FSM diagram + sequence diagram: `../../datn-agent-skills/project_setup/firmware_architecture_design.md`
- Cập nhật kiến trúc mới nhất: `../../datn-agent-skills/project_setup/firmware_architecture_update_080526.md`
- Kế hoạch refactor: `../../datn-agent-skills/project_setup/firmware_restructure_plan_v2.md`
- README từng component: `components/<tên>/README.md`
