# Component: drv_battery

> **Cập nhật cuối (Timestamp):** 2026-06-30

## 1. Mục đích (Purpose / Why)
Module `drv_battery` là một driver cấp thấp chịu trách nhiệm giao tiếp với bộ ADC của ESP32-S3 để đọc mức điện áp của pin Li-ion (3.7V - 4.2V) cung cấp cho hệ thống.
Thiết kế của nó được bóc tách thành một component độc lập nhằm giấu đi sự phức tạp của phần cứng (như các thông số ADC, hiệu chuẩn curve-fitting, và cầu phân áp) khỏi các tầng logic phía trên (như `svc_cloud` hay `sys_manager`).

## 2. Cơ chế cốt lõi (How it works)
* **Kỹ thuật Cầu phân áp:** Vì điện áp pin đầy (4.2V) vượt quá dải đo của ADC (tối đa ~3.1V với atten 12dB), phần cứng sử dụng cầu phân áp chia đôi điện áp (`s_ratio = 2.0`). Driver sẽ lấy giá trị ADC đọc được nhân ngược với tỷ lệ này để ra điện áp thực. Hiệu chuẩn curve-fitting (nếu hỗ trợ như trên S3) được gọi qua `esp_adc_cal`.
* **Lọc nhiễu (Median + EMA Filter):** Điện áp pin rất nhiễu do sụt áp khi tải nặng (voltage dips do 4G phát sóng) và các gai nhiễu dương (spikes). Cứ 2 giây driver lưu 1 mẫu ADC vào mảng 5 phần tử. Sau đó áp dụng **Lọc Trung Vị (Median Filter)** để loại bỏ hoàn toàn nhiễu dị thường (outliers), và đưa qua **Lọc Hàm Mũ (EMA - Exponential Moving Average)** (với $\alpha = 0.1$) để làm mượt dài hạn.
* **Tính phần trăm (%) bằng LUT:** Thay vì ánh xạ tuyến tính thô, driver sử dụng Bảng tra (Look-up Table - LUT) 11 điểm mô phỏng đường cong xả thực tế của pin Li-ion (từ 3.0V = 0% đến 4.2V = 100%). Giá trị % cuối cùng được nội suy tuyến tính (Linear Interpolation) giữa 2 mốc điện áp trong bảng.
* **Logic Monotonic (Chỉ giảm, không tăng):** Tránh hiện tượng % pin nhảy lên/xuống liên tục khi đóng ngắt tải. Biến % pin báo cáo chỉ được phép giảm. Nếu % tính ra cao hơn % hiện tại, nó phải giữ liên tục trên mức đó hơn 1 phút mới được phép cập nhật ngược lên (để tương thích cả với việc cắm sạc).

## 3. Đa nhiệm & Tài nguyên (Concurrency & Resources)
* **Không tốn Task riêng:** Thay vì chạy một FreeRTOS Task (gây lãng phí Stack), module sử dụng `esp_timer` (Hardware Timer) chạy chìm ở chế độ ngắt mềm định kỳ (2000ms/lần) để thu thập mẫu vào mảng.
* **Bảo vệ dữ liệu (Thread-Safety):** Quá trình Timer ghi vào mảng và hàm bên ngoài đọc từ mảng được bảo vệ bởi `xSemaphoreCreateMutex()`, giúp thao tác an toàn ngay cả khi truy cập từ nhiều Task khác nhau.

## 4. Luồng giao tiếp (Data Flow)
* **Khởi tạo:** `app_main.c` gọi hàm init, tự động ánh xạ GPIO do người dùng cấu hình thành ADC Unit & Channel tương ứng thông qua `adc_oneshot_io_to_channel`.
* **Cung cấp dữ liệu:** Module không tự động "bắn" (push) sự kiện ra ngoài. Khi `svc_cloud` cần đóng gói bản tin Telemetry, nó sẽ chủ động gọi hàm `drv_battery_read_percent()` (pull data) để lấy % pin hiện tại.

## 5. Public API
- **`esp_err_t drv_battery_init(int adc_gpio, float divider_ratio)`** — cấu hình ADC (atten 12dB, 12-bit) + hiệu chuẩn curve-fitting. `divider_ratio` = hệ số cầu phân áp (2× 100kΩ → `2.0`).
- **`int drv_battery_read_mv(void)`** — trả về điện áp pin (mV) hiện tại đã qua bộ lọc Median + EMA; `-1` nếu lỗi.
- **`int drv_battery_read_percent(void)`** — trả về % pin Li-ion 1 cell (nội suy qua LUT 11 điểm + áp dụng thuật toán monotonic); `-1` nếu lỗi.
