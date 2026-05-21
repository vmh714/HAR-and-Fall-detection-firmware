# 📱 Component: drv_a7680c (A7680C 4G LTE Hardware Driver)

> **Mục đích**: Lớp driver phần cứng cấp thấp quản lý nguồn, điều khiển chân Reset vật lý của module viễn thông 4G LTE SIMCOM A7680C.
> **Cập nhật cuối (Timestamp)**: 2026-05-21T15:07:00+07:00
> **Trạng thái**: Bản phát triển ban đầu (Phục vụ Phase 2)

---

## 1. THIẾT KẾ PHẦN CỨNG & QUẢN LÝ NGUỒN ĐIỆN
Module **A7680C** là dòng modem siêu nhỏ hỗ trợ mạng LTE Cat-1. Khi thiết kế mạch và tích hợp, lập trình viên bắt buộc phải nắm rõ các đặc tính phần cứng sau:

### 1.1 Yêu cầu cấp nguồn cực kỳ quan trọng
*   **Điện áp hoạt động (VBAT)**: **3.4V - 4.2V** (Khuyên dùng: **3.8V**).
*   **Nguồn cấp**: Phải đấu nối **trực tiếp** từ Pin Li-ion (3.7V) hoặc qua một nguồn Buck chất lượng cao có khả năng xả dòng tức thời lớn. Tuyệt đối **không** cấp nguồn qua LDO 3.3V thông thường của mạch nạp ESP32 do LDO không đủ dòng.
*   **Dòng tiêu thụ đỉnh (Burst Current)**: Trong quá trình truyền nhận dữ liệu hoặc dò sóng di động, A7680C có thể tiêu thụ các dòng điện đỉnh đột ngột từ **300mA - 500mA** (và có thể lên tới 2A trong khoảng vài micro giây). Nếu nguồn yếu, sụt áp nguồn sẽ khiến module lập tức reset liên tục (Boot loop).

### 1.2 Giao tiếp tín hiệu
*   **Cổng truyền thông**: UART1 hoặc UART2 của ESP32-S3 (Baudrate mặc định: `115200bps`).
*   **Chân Reset phần cứng (`A7680C_RESET_PIN`)**: GPIO 18. Hoạt động tích cực mức thấp (Active Low).

---

## 2. CHỨC NĂNG CỦA CÁC API
Các hàm công bố trong [drv_a7680c.h](file:///d:/datn/firmware/components/drv_a7680c/include/drv_a7680c.h):

### 2.1 Khởi tạo GPIO điều khiển
```c
void drv_a7680c_init(void);
```
Cấu hình chân ngắt/điều khiển GPIO 18 ở chế độ Output, ngắt pull-up/pull-down để tránh mức logic không xác định làm reset nhầm module khi khởi động. Đưa chân RESET lên mức cao (1).

### 2.2 Reset phần cứng
```c
void drv_a7680c_reset(void);
```
*   **Mô tả**: Kéo chân Reset phần cứng xuống mức thấp (0) trong vòng **500ms**, sau đó kéo ngược lại mức cao (1).
*   **Mục đích**: Khôi phục trạng thái hoạt động của module di động khi phát hiện đơ treo tập lệnh AT hoặc mất kết nối mạng cục bộ kéo dài không thể phục hồi bằng phần mềm.

### 2.3 Khởi động nguồn
```c
void drv_a7680c_power_on(void);
```
Kích hoạt chu trình khởi động nguồn của chip (phát tín hiệu Reset vật lý lúc boot hệ thống).

---

## 3. HƯỚNG DẪN MỞ RỘNG (PHASE 2 WORKFLOW)
Khi bắt đầu triển khai Phase 2:
1.  Thiết lập đường truyền UART sử dụng driver `driver/uart.h` của ESP-IDF để gửi lệnh cấu hình AT command.
2.  Tích hợp cấu hình thiết lập mạng GPRS thông qua lệnh AT `AT+CGDCONT=1,"IP","[APN_NHA_MANG]"`.
3.  Cấu hình luồng dữ liệu PPP (Point-to-Point Protocol) để giao tiếp mạng IP thuần.
---
