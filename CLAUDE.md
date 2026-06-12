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

| State | Mô tả |
|---|---|
| `STATE_INIT` | Khởi tạo phần cứng, driver, services |
| `STATE_CONNECTING` | Chờ mạng + MQTT sẵn sàng, tự retry |
| `STATE_NORMAL` | **Production**: AI inference liên tục, gửi telemetry 1 phút/lần, alert ngay khi ngã |
| `STATE_STREAMING` | **Data collection**: bỏ qua AI, batch raw IMU → MQTT để thu thập dataset train |
| `STATE_OTA` | Nhận firmware mới → restart |

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

## MQTT Topics

| Topic | Hướng | Mô tả |
|---|---|---|
| `v1/devices/{id}/alerts` | Publish | Cảnh báo té ngã (QoS 1) |
| `v1/devices/{id}/telemetry` | Publish | Telemetry định kỳ (pin, bước chân, ...) |
| `v1/devices/{id}/imu_stream` | Publish | Raw IMU batch (chỉ STATE_STREAMING) |
| `v1/devices/{id}/commands` | Subscribe | Nhận lệnh từ backend (start/stop stream, OTA) |

## Quy ước quan trọng
- **Không gọi trực tiếp giữa các svc_**: mọi giao tiếp phải qua `esp_event_post()` → `sys_manager` xử lý
- **Sliding Window**: 200 mẫu (2s ở 100Hz), trượt 50 mẫu (0.5s) — không thay đổi mà không retrain model
- **Cooldown chống spam**: `svc_ai` im lặng 10–20 giây sau mỗi lần phát alert
- **`lib_model/model_data.cc`**: file này được sinh tự động từ TFLite — KHÔNG sửa tay

## Tài liệu tham khảo chi tiết
- Kiến trúc đầy đủ + FSM diagram + sequence diagram: `../../datn-agent-skills/project_setup/firmware_architecture_design.md`
- Cập nhật kiến trúc mới nhất: `../../datn-agent-skills/project_setup/firmware_architecture_update_080526.md`
- Kế hoạch refactor: `../../datn-agent-skills/project_setup/firmware_restructure_plan_v2.md`
- README từng component: `components/<tên>/README.md`
