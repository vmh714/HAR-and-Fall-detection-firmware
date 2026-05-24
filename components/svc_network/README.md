# 🌐 Component: svc_network (Physical Network Service)

> **Mục đích**: Chịu trách nhiệm quản lý tầng kết nối mạng vật lý. Đảm bảo thiết bị luôn được kết nối với Internet thông qua WiFi Station (STA) hoặc tự động khôi phục kết nối khi mất sóng. Cung cấp API chờ cấp phát IP trước khi các service khác hoạt động.
> **Cập nhật cuối (Timestamp)**: 2026-05-21T16:04:00+07:00
> **Trạng thái**: Hoạt động ổn định

---

## 1. NGUYÊN LÝ HOẠT ĐỘNG & SỰ KIỆN MẠNG
Dịch vụ mạng hoạt động theo mô hình bất đồng bộ hướng sự kiện. Tự động lắng nghe và khôi phục khi mất kết nối tín hiệu vật lý.

### 1.1 Khởi tạo mạng song song (Parallel Boot Sequence)
Để giảm thiểu thời gian khởi động thiết bị (Boot Time), quá trình kết nối mạng WiFi chạy song song với quá trình hiệu chuẩn IMU cảm biến (do I2C và RF độc lập tài nguyên):
1.  `app_main` khởi tạo nền tảng Netif và Event Loop chung.
2.  **Nhánh 1**: IMU bắt đầu I2C Init và Calibrate (~2-6 giây tĩnh).
3.  **Nhánh 2**: `svc_network` khởi tạo WiFi STA, kết nối bộ thu phát sóng RF và chặn chờ (blocking) cho tới khi được router cấp phát địa chỉ IP.
4.  Cả hai nhánh đồng bộ hóa tại Sync Barrier. Event `NET_EVT_WIFI_CONNECTED` được phát ra Event Loop.

### 1.2 Event Base
*   `NET_EVENT`: Quản lý bởi Event Loop trung tâm `sys_manager`.
    *   `NET_EVT_WIFI_CONNECTED`: Phát ra khi kết nối WiFi thành công và đã nhận IP hợp lệ.
    *   `NET_EVT_CELLULAR_CONNECTED`: Dành cho Phase 2 khi chuyển sang LTE.
    *   `NET_EVT_DISCONNECTED`: Phát ra khi mất kết nối mạng liên tục, báo hiệu cho các hệ thống khác tạm dừng hoạt động.

---

## 2. HƯỚNG DẪN SỬ DỤNG API
Các hàm công bố trong [svc_network.h](file:///d:/datn/firmware/components/svc_network/include/svc_network.h):

### 2.1 Khởi tạo dịch vụ mạng (Blocking Init)
```c
esp_err_t svc_network_init(const char *ssid, const char *pass);
```
*   **Mô tả**: Thiết lập WiFi STA, tiến hành quét và kết nối với Access Point. Hàm này sẽ **chặn (block)** luồng gọi cho đến khi nhận địa chỉ IP thành công.
*   **Điều kiện tiên quyết**: Caller bắt buộc phải gọi `nvs_flash_init()`, `esp_netif_init()`, và `esp_event_loop_create_default()` trước khi gọi hàm này.

### 2.2 Kiểm tra trạng thái kết nối
```c
bool svc_network_is_connected(void);
```
*   Trả về `true` nếu thiết bị hiện đang duy trì kết nối mạng vật lý ổn định (đã có IP).

---

## 3. KẾ HOẠCH TIẾN HÓA: WIFI ➔ 4G LTE A7680C (PHASE 2)
Để thiết bị hoạt động độc lập ngoài trời (Wearable), tầng vật lý sẽ chuyển đổi từ WiFi sang 4G LTE:
1.  Tích hợp `drv_a7680c` vào `svc_network`.
2.  Sử dụng giao tiếp UART gửi lệnh AT command hoặc cấu hình PPPoS (Point-to-Point Protocol over Serial) thông qua component `esp_modem` của ESP-IDF.
3.  Khi PPPoS thiết lập thành công IP qua mạng di động, `svc_network` sẽ phát `NET_EVT_CELLULAR_CONNECTED` thay cho WiFi. Các lớp ứng dụng bên trên (như `svc_cloud`) hoàn toàn không cần thay đổi code.
