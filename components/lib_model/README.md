# Component: lib_model

> **Cập nhật cuối (Timestamp):** 2026-06-30
> **Trạng thái:** Hoạt động ổn định

---

## 1. PUBLIC API

Các biến được công bố trong [model_data.h](include/model_data.h):

### 1.1 Khai báo Extern
```c
extern const unsigned char g_model_data[];
extern const unsigned int g_model_data_len;
```
*   `g_model_data`: Mảng byte chứa toàn bộ nội dung model TensorFlow Lite (bắt đầu bằng magic number `TFL3`).
*   `g_model_data_len`: Kích thước của mảng byte.

> **Lưu ý**: Cả hai biến đều được định nghĩa với từ khóa `const` để trình biên dịch đặt chúng vào bộ nhớ Flash (DROM) thay vì nạp lên RAM.

---

## 2. Mục đích (Purpose / Why)
Component chứa **mô hình TFLite (`.tflite`) đã quantize INT8** được nhúng trực tiếp vào firmware dưới dạng C++ byte array (`model_data.cc`). Việc nhúng mã hex trực tiếp (thay vì dùng hệ thống file SPIFFS/FATFS) giúp đơn giản hóa quá trình OTA và tải mô hình lên TFLite Micro một cách an toàn nhất, tránh rủi ro đọc file hỏng.

## 3. Cơ chế cốt lõi (How it works): Đặc Tả Mô Hình
File `model_data.cc` không chỉ chứa trọng số mà chứa **trọn vẹn kiến trúc mạng**, tham số quantization và tensor metadata:
- **Kiến trúc**: ResNet-1D (phiên bản v25) — Conv1D(16) → 4×(ResNet Block + SE Block) → GAP+GMP → Dense.
- **Input**: shape `(200, 6)` INT8 — cửa sổ 200 mẫu × 6 trục (accel + gyro).
- **Output**: shape `(5)` INT8 — 5 lớp: `Walk`, `Run`, `Idle`, `Transition`, `Fall`.
- **Kích thước**: `g_model_data_len = 81384` bytes (~79.5 KB).
- **Lượng tử hóa**: INT8 (full integer quantization).

File này được sinh tự động từ file `.tflite` bằng script chuyển đổi tùy biến ở Jupyter Notebook (`convert_tflite_to_cc.py`), sinh ra mảng `const g_model_data` (KHÔNG dùng `xxd -i`, vốn sinh mảng không có `const` làm tốn RAM).

## 4. Đa nhiệm & Tài nguyên (Concurrency & Resources)
* Do mảng `g_model_data` mang tiền tố `const`, Linker sẽ nhúng trực tiếp mảng vào phân vùng Flash (DROM) của bộ vi điều khiển.
* Trình thông dịch `MicroInterpreter` sẽ đọc trực tiếp từ Flash qua Data Bus (memory-mapped flash), **không** tiêu tốn RAM (chỉ tốn dung lượng Flash chứa chương trình).

## 5. Luồng giao tiếp (Data Flow)
* **Consumer (Người tiêu thụ)**: Mảng `g_model_data` được gọi độc quyền bởi component `lib_tinyml` qua hàm `tflite::GetModel(g_model_data)` để khởi tạo. Không có luồng giao tiếp dữ liệu động ở runtime (thuần tĩnh).
