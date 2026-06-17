# Component: lib_model

## Tổng Quan
Component chứa **model TFLite (`.tflite`) đã quantize INT8** được nhúng dưới dạng C++ byte array. File `model_data.cc` lưu toàn bộ model — bao gồm kiến trúc mạng, trọng số (weights) và tham số quantization — với magic number `TFL3` ở đầu file. Đây không chỉ là mảng trọng số mà là **trọn vẹn một model TFLite Micro** sẵn sàng nạp vào interpreter.

Component gồm:
- `model_data.cc` — model TFLite (C++ byte array).
- `include/model_data.h` — khai báo extern API.
- `CMakeLists.txt` — đăng ký component với ESP-IDF.

## Đặc Tả Model
- **Kiến trúc**: ResNet-1D (phiên bản v25) — Conv1D(16) → 4×(ResNet Block + SE Block) → GAP+GMP → Dense.
- **Input**: shape `(200, 6)` INT8 — cửa sổ 200 mẫu × 6 trục (accel + gyro).
- **Output**: shape `(5)` INT8 — 5 lớp: `Walk`, `Run`, `Idle`, `Transition`, `Fall`.
- **Kích thước**: `g_model_data_len = 81384` bytes (~79.5 KB).
- **Lượng tử hóa**: INT8 (full integer quantization).

## Các API/Biến Chính
```c
extern const unsigned char g_model_data[];
extern const unsigned int g_model_data_len;
```
(khai báo tại `include/model_data.h:8-9`, đều là `const`).

**Consumer**: được dùng tại `lib_tinyml` qua `tflite::GetModel(g_model_data)` để khởi tạo `MicroInterpreter`.

*Hoạt động*: File được sinh tự động từ file `.tflite` bằng script chuyển đổi tùy biến (ví dụ `convert_tflite_to_cc.py`) — sinh ra biến `const g_model_data` kèm comment mô tả kiến trúc ở đầu file (KHÔNG dùng `xxd -i`, vốn sinh mảng không có `const` và đặt tên biến theo đường dẫn file). Mảng `g_model_data` được Linker nhúng trực tiếp vào phân vùng Flash (DROM), không tiêu tốn RAM, để `MicroInterpreter` đọc trực tiếp.

> **Cập nhật cuối (Timestamp)**: 2026-06-17
</content>
