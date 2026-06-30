# Component: lib_pedometer

> **Cập nhật cuối (Timestamp):** 2026-06-30
> **Trạng thái:** Hoạt động ổn định

---

## 1. PUBLIC API

Các hàm công bố trong [pedometer.h](include/pedometer.h):

### 1.1 Khởi tạo thuật toán
```c
void pedometer_init(pedometer_t *ped, float fs_hz);
```
*   **Mô tả**: Tính toán và nạp các hệ số lọc Band-pass theo tần số lấy mẫu `fs_hz` truyền vào. Đặt giá trị thời gian trơ mặc định là 250ms (chống đếm đúp 4 bước/s) và ngưỡng đỉnh-đỉnh `min_p2p = 0.15g`.

### 1.2 Cập nhật mẫu gia tốc (Process)
```c
bool pedometer_process(pedometer_t *ped, float ax, float ay, float az);
```
*   **Mô tả**: Đưa vào một mẫu gia tốc 3 trục (đơn vị **g**). Hàm trả về `true` nếu mẫu hiện tại thỏa mãn đủ điều kiện cắt ngưỡng và kết thúc một bước chân hợp lệ.

---

## 2. Mục đích (Purpose / Why)
Thư viện **thuần toán học** để phát hiện và đếm bước chân (pedometer) từ dữ liệu gia tốc. Không phụ thuộc vào phần cứng cảm biến, FreeRTOS hay bộ dự đoán hành vi HAR. Việc quyết định chỉ đếm bước khi đi bộ (`Walk`) hay chạy (`Run`) hoàn toàn do tầng ứng dụng (`svc_imu`) kiểm soát.

## 3. Cơ chế cốt lõi (How it works)
Thuật toán dựa trên 4 bước chính:
1. **Trích xuất Magnitude**: `|a| = √(ax² + ay² + az²)` — Tính độ lớn vector gia tốc 3D, giúp kết quả bất biến với hướng đeo của cảm biến (người dùng có thể đeo ngược, nghiêng).
2. **Lọc Band-pass 0.5–3.5Hz**: Áp dụng High-pass 1 bậc để loại bỏ thành phần trọng lực / trôi từ từ, kết hợp Low-pass 1 bậc để khử nhiễu rung tần số cao. Quá trình này giúp tách chính xác dải nhịp bước tự nhiên (đi bộ ~1.5–2Hz, chạy ~2.5–3.3Hz).
3. **Ngưỡng động (Dynamic Threshold)**: Tính trung điểm giữa Envelope Max và Envelope Min. Ngưỡng bám theo biên độ chuyển động nhanh chóng và suy giảm chậm dần sau ~2s. Cơ chế này tự thích nghi hoàn hảo với bước chân dài/ngắn, mạnh/nhẹ của từng cá nhân.
4. **Peak-detect (Xác nhận bước)**: Một bước được công nhận khi và chỉ khi:
   - Tín hiệu cắt qua ngưỡng động từ trên xuống dưới (Cạnh xuống).
   - Biên độ đỉnh-đỉnh (Peak-to-Peak) `> min_p2p`.
   - Đã vượt qua khoảng **thời gian trơ** (`refractory_samples`) kể từ bước trước, ngăn chặn tình trạng một sải chân dài sinh ra nhiều đỉnh phụ khiến đếm đúp (double counting).

## 4. Đa nhiệm & Tài nguyên (Concurrency & Resources)
* Hoàn toàn độc lập nền tảng (platform-independent), chạy trực tiếp trên luồng của người gọi (caller thread).
* Cấu trúc `pedometer_t` lưu trữ nội bộ (state) bằng các biến số thực tĩnh, hoàn toàn an toàn (thread-safe nếu không truy cập chéo).

## 5. Luồng giao tiếp (Data Flow)
* **Khuyến nghị đầu vào**: Cấp tọa độ **accel THÔ** (không qua bộ lọc làm mượt Kalman 1D) để tín hiệu đỉnh va chạm gót chân hiện rõ nét nhất.
* **Luồng gọi hàm**: `pedometer_process` cần được gọi liên tục trên **mỗi mẫu** lấy được (ngay cả khi người dùng đứng yên). Việc ngắt quãng gọi hàm sẽ gây ra các bước sóng "quá độ" (transient) làm thuật toán đếm sai khi hoạt động lại.
* **Tích hợp**: Caller có thể chủ động sửa trực tiếp `ped->refractory_samples` (VD: Walk ~300ms, Run ~200ms) hoặc `ped->min_p2p` trong lúc chạy để tinh chỉnh theo mô hình dáng đi (Gait model).
