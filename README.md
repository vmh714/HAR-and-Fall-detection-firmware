# Wearable HAR & Post-Impact Fall Detection Firmware

## Tổng Quan Hệ Thống (System Overview)
Hệ thống giám sát vận động (HAR) và phát hiện té ngã trên thiết bị đeo, sử dụng vi điều khiển **Seeed Studio XIAO ESP32S3** (8MB PSRAM/Flash). Hệ thống tích hợp cảm biến gia tốc/góc nghiêng MPU6050, thực hiện suy luận học máy cục bộ (TinyML/Edge AI) với độ trễ cực thấp (32ms), và sẽ giao tiếp qua WiFi/4G LTE để cảnh báo khẩn cấp (MQTT).

## Lịch Sử và Kế Hoạch Triển Khai (Implementation Roadmap)

### Các Phase Đã Hoàn Thành
*   **Phase 0 (Init & Prototyping)**: Giai đoạn sơ khai kiểm thử phần cứng. Xây dựng hai file mã nguồn nguyên khối (legacy code) là `esp32s3_wifi_mqtt.c` và `esp32s3_mpu6050.c` dùng để viết nháp và test driver cho cảm biến MPU6050 cũng như các kết nối cơ bản WiFi, MQTT. Đây là bước đệm quan trọng trước khi toàn bộ dự án được "đập đi xây lại" theo kiến trúc phân tách Component.
*   **Phase 1 (Network & Infrastructure)**: Xây dựng máy trạng thái hữu hạn `sys_manager` (FSM), tách biệt module mạng WiFi (`svc_network`) và MQTT (`svc_cloud`), thiết lập hệ thống Event Loop liên lạc giữa các task.
*   **Phase 2 (TinyML Integration)**: Chuẩn bị môi trường PSRAM, cấu hình thư viện `lib_tinyml` dựa trên tflite-micro để chạy mô hình AI nội bộ.
*   **Phase 3 (Data Collection Pipeline)**: Tích hợp cảm biến MPU6050, xây dựng luồng lấy mẫu chính xác 100Hz dùng PCNT hardware, lọc nhiễu bằng Kalman Filter (`lib_kalman`), và đẩy data 2s-window vào queue.
*   **Phase 4 (AI Inference & Posture Logic)**: Cấu hình OPI PSRAM 80MHz & ESP-NN, đưa thời gian suy luận (inference time) xuống 32ms. Hoàn thành logic tính toán tư thế dựa trên góc Pitch (Stand/Sit/Lying) tại `svc_ai`.

### Kế Hoạch Sắp Tới (Upcoming Phases)
*   **Phase 4.1 (AI Logic + MQTT Alert)**: Ghép nối cờ báo động từ `svc_ai` vào `sys_manager` để lập tức kích hoạt bản tin MQTT SOS (Fall Detected) trên `svc_cloud` kèm thời gian delay (cooldown).
*   **Phase 4.2 (Tiết kiệm điện - Light Sleep)**: Chuyển cấu hình Clock (I2C/PCNT) sang XTAL/RTC, cấu hình Automatic Light Sleep (Tickless Idle) để ESP32-S3 tự động ngủ sau mỗi nhịp báo cáo giúp tối đa hóa thời lượng pin.
*   **Phase 5 (4G LTE Integration)**: Bổ sung module SIM A7680C qua UART, chuyển đổi `svc_network` sang dùng kết nối PPP (LwIP) để thiết bị hoạt động độc lập không cần WiFi.
*   **Phase 5.1 (OTA Updates)**: Triển khai khả năng nâng cấp firmware từ xa (Over-The-Air) qua hạ tầng mạng 4G LTE.

## Cấu Trúc Mã Nguồn (Components)
Hệ thống áp dụng mô hình phân tách **Service - Driver**:
- Các `drv_` giao tiếp trực tiếp phần cứng, không in log nghiệp vụ.
- Các `svc_` xử lý logic nghiệp vụ trung tâm, in log hệ thống, quản lý FreeRTOS tasks.
- Các `lib_` chứa các thuật toán tính toán toán học độc lập (platform-independent).

Vui lòng xem `README.md` trong từng thư mục con thuộc mục `components/` để nắm rõ nguyên lý hoạt động của từng module.
