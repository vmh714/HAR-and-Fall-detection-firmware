# 🌐 Component: svc_network (Physical Network Service)

> **Mục đích**: Chịu trách nhiệm quản lý tầng kết nối mạng vật lý. Đảm bảo thiết bị luôn được kết nối với Internet thông qua WiFi Station (STA) và tự động khôi phục kết nối khi mất sóng. Khởi tạo theo cơ chế bất đồng bộ (non-blocking), thông báo trạng thái cấp phát IP cho các service khác qua Event Loop.
> **Cập nhật cuối (Timestamp)**: 2026-06-17
> **Trạng thái**: Hoạt động ổn định

---

## 1. NGUYÊN LÝ HOẠT ĐỘNG & SỰ KIỆN MẠNG
Dịch vụ mạng hoạt động theo mô hình bất đồng bộ hướng sự kiện. Tự động lắng nghe và khôi phục khi mất kết nối tín hiệu vật lý.

### 1.1 Khởi tạo non-blocking (Asynchronous Boot)
Hàm `svc_network_init()` **không chặn (non-blocking)**: nó cấu hình và gọi `esp_wifi_start()` rồi trả về `ESP_OK` ngay lập tức, không chờ kết nối hay cấp phát IP. Nhờ đó quá trình kết nối WiFi diễn ra song song với các tác vụ khởi động khác (ví dụ hiệu chuẩn IMU) mà không cần Sync Barrier:
1.  `app_main` khởi tạo nền tảng Netif và Event Loop chung (xem mục Điều kiện tiên quyết).
2.  `svc_network_init()` đăng ký event handler, khởi động WiFi STA và trả về ngay.
3.  Toàn bộ quá trình kết nối được xử lý **bất đồng bộ** trong `wifi_event_handler`:
    *   `WIFI_EVENT_STA_START` → gọi `esp_wifi_connect()`.
    *   `IP_EVENT_STA_GOT_IP` → đặt cờ kết nối và phát `NET_EVT_WIFI_CONNECTED`.
4.  Caller **không** được giả định mạng đã sẵn sàng sau khi `svc_network_init()` trả về. Phải kiểm tra trạng thái qua `svc_network_is_connected()` hoặc lắng nghe event `NET_EVT_WIFI_CONNECTED` trên Event Loop.

### 1.2 Cơ chế tự kết nối lại (Auto-reconnect)
Mỗi khi xảy ra `WIFI_EVENT_STA_DISCONNECTED`, handler lập tức:
1.  Đặt cờ kết nối về `false` và phát `NET_EVT_DISCONNECTED`.
2.  Gọi lại `esp_wifi_connect()` để thử kết nối lại — **không giới hạn số lần thử** nhằm đảm bảo tính sẵn sàng.

> **Lưu ý**: Do retry không giới hạn và mỗi lần ngắt đều phát event, `NET_EVT_DISCONNECTED` có thể được phát **rất thường xuyên** khi sóng yếu/chập chờn. Các consumer cần xử lý debounce/lọc trùng nếu cần. Macro `MAXIMUM_RETRY` (=5) hiện là dead code, không được dùng để giới hạn retry.

### 1.3 Event Base
*   `NET_EVENT`: Quản lý bởi Event Loop trung tâm `sys_manager`.
    *   `NET_EVT_WIFI_CONNECTED`: Phát ra khi kết nối WiFi thành công và đã nhận IP hợp lệ.
    *   `NET_EVT_CELLULAR_CONNECTED`: Được định nghĩa ở `sys_manager` cho Phase 2 (LTE), nhưng **component này chưa có code nào phát ra event này**.
    *   `NET_EVT_DISCONNECTED`: Phát ra mỗi lần mất kết nối WiFi (có thể lặp lại liên tục), báo hiệu cho các hệ thống khác tạm dừng hoạt động.

---

## 2. HƯỚNG DẪN SỬ DỤNG API
Các hàm công bố trong [svc_network.h](include/svc_network.h):

### 2.1 Khởi tạo dịch vụ mạng (Non-blocking Init)
```c
esp_err_t svc_network_init(const char *ssid, const char *pass);
```
*   **Mô tả**: Thiết lập WiFi STA và bắt đầu quá trình kết nối tới Access Point. Hàm **không chặn (non-blocking)** — sau khi gọi `esp_wifi_start()`, hàm trả về `ESP_OK` ngay lập tức. Việc kết nối và cấp phát IP diễn ra bất đồng bộ ở phía sau (xem mục 1.1). Caller phải dùng `svc_network_is_connected()` hoặc event `NET_EVT_WIFI_CONNECTED` để biết khi nào mạng sẵn sàng.
*   **Ràng buộc authmode**: Cấu hình cố định `WIFI_AUTH_WPA_WPA2_PSK`. Do đó các AP chỉ hỗ trợ WPA3-only hoặc mạng mở (open/no-password) sẽ **không** kết nối được.
*   **Điều kiện tiên quyết**: Caller bắt buộc phải gọi `nvs_flash_init()`, `esp_netif_init()`, và `esp_event_loop_create_default()` trước khi gọi hàm này. (Component tự gọi `esp_netif_create_default_wifi_sta()` bên trong, không cần caller tạo netif STA.)

### 2.2 Kiểm tra trạng thái kết nối
```c
bool svc_network_is_connected(void);
```
*   Trả về `true` nếu thiết bị hiện đang duy trì kết nối mạng vật lý ổn định (đã có IP).

---

## 3. KẾ HOẠCH TIẾN HÓA: WIFI ➔ 4G LTE A7680C (PHASE 2 — CHƯA HIỆN THỰC)
> **Trạng thái**: Toàn bộ phần dưới đây là **kế hoạch tương lai**. Component `svc_network` hiện tại **chỉ hỗ trợ WiFi STA**; chưa có code 4G/LTE/PPPoS nào được hiện thực.

Để thiết bị hoạt động độc lập ngoài trời (Wearable), tầng vật lý sẽ chuyển đổi từ WiFi sang 4G LTE:
1.  Tích hợp `drv_a7680c` vào `svc_network`.
2.  Sử dụng giao tiếp UART gửi lệnh AT command hoặc cấu hình PPPoS (Point-to-Point Protocol over Serial) thông qua component `esp_modem` của ESP-IDF.
3.  Khi PPPoS thiết lập thành công IP qua mạng di động, `svc_network` sẽ phát `NET_EVT_CELLULAR_CONNECTED` thay cho WiFi. Các lớp ứng dụng bên trên (như `svc_cloud`) hoàn toàn không cần thay đổi code.
