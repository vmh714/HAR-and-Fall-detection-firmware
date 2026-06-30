# Wearable HAR & Post-Impact Fall Detection Firmware

## Tổng Quan Hệ Thống (System Overview)
Hệ thống giám sát vận động (HAR) và phát hiện té ngã trên thiết bị đeo, sử dụng vi điều khiển **Seeed Studio XIAO ESP32S3** (8MB PSRAM/Flash). Hệ thống tích hợp cảm biến gia tốc/góc nghiêng MPU6050, thực hiện suy luận học máy cục bộ (TinyML/Edge AI) với độ trễ cực thấp (32ms), giao tiếp qua WiFi và 4G LTE (A7680C) để đẩy dữ liệu (Telemetry) và cảnh báo khẩn cấp (Alert) qua giao thức MQTT.

## Các Tính Năng Cốt Lõi (Core Features)
- **Kiến trúc Sự kiện (Event-Driven FSM)**: Quản lý vòng đời hoạt động bằng máy trạng thái hữu hạn, giao tiếp liên luồng qua `esp_event` và FreeRTOS `xQueue`. Tự động phục hồi lỗi (Watchdog, Hardware Error Reboot).
- **Thu thập dữ liệu MPU6050 (100Hz)**: Đọc FIFO bằng ngắt cứng PCNT, tự động xử lý trôi lệch (drift) và xả tràn (overflow). Bộ lọc Kalman 1D và 2-state ước lượng tư thế Roll.
- **TinyML Edge AI (ESP-NN)**: Tích hợp TensorFlow Lite Micro chạy mô hình INT8 (CNN 1D). Kỹ thuật trượt cửa sổ dữ liệu liên tục 0.5s/2s cho phép nhận diện 5 lớp hoạt động (Walk, Run, Idle, Trans, Fall).
- **Chống Báo Động Giả (False Alarm Gating)**:
  - **Pre-Impact Posture Gating**: Tự động từ chối cú ngã nếu thiết bị không đeo trên người (ví dụ: đang nằm bẹp trên bàn >3s bị va đập).
  - **Post-Impact Confirmation FSM**: Chờ 4s sau cú ngã để quan sát sự phục hồi tư thế. Chỉ báo động nếu người dùng hoàn toàn nằm bất động, hủy báo động nếu tự đứng lên được.
- **Kết nối linh hoạt (WiFi / 4G LTE)**: Tự động phát hiện module SIM (Auto-detect) qua UART sniff. Giao thức PPP qua LwIP cho mạng Cellular siêu mượt.
- **Pedometer & Battery Monitor**: Đếm số bước đi bộ/chạy độc lập dựa trên tín hiệu AI. Đo pin dùng bảng tra (LUT), lọc Median, EMA và logic tuyến tính hóa đơn điệu (Monotonic) chống nhảy % pin.

## Cấu Trúc Mã Nguồn (Components)
Hệ thống áp dụng mô hình phân tách **Service - Driver**:
- Các `drv_` giao tiếp trực tiếp phần cứng, không chứa log nghiệp vụ (VD: `drv_mpu6050`, `drv_a7680c`, `drv_battery`).
- Các `svc_` xử lý logic nghiệp vụ trung tâm, in log hệ thống, quản lý FreeRTOS tasks (VD: `sys_manager`, `svc_network`, `svc_cloud`, `svc_imu`, `svc_ai`).
- Các `lib_` chứa thuật toán toán học/xử lý tín hiệu độc lập không phụ thuộc platform (VD: `lib_kalman`, `lib_pedometer`).

> *Vui lòng tham khảo các tệp kiến trúc trong `datn_agent_skills/project_setup/architecture` hoặc các tệp `README.md` bên trong từng thư mục con để xem chi tiết.*
