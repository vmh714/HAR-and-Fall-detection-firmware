# 🌐 Component: svc_network (Physical Network Service)

> **Cập nhật cuối (Timestamp):** 2026-06-30
> **Trạng thái:** Hoạt động ổn định (auto-detect 4G bằng sniff bus UART — đã verify)

---

## 1. PUBLIC API

Các hàm công bố trong [svc_network.h](include/svc_network.h):

### 1.1 Khởi tạo dịch vụ mạng (Non-blocking Init)
```c
esp_err_t svc_network_init(const char *ssid, const char *pass);
```
*   **Mô tả**: Thiết lập WiFi STA và bắt đầu quá trình kết nối tới Access Point. Hàm **không chặn (non-blocking)** — sau khi gọi `esp_wifi_start()`, hàm trả về `ESP_OK` ngay lập tức. Việc kết nối và cấp phát IP diễn ra bất đồng bộ. Caller phải dùng `svc_network_is_connected()` hoặc event `NET_EVT_WIFI_CONNECTED` để biết khi nào mạng sẵn sàng.
*   **Ràng buộc authmode**: Cấu hình cố định `WIFI_AUTH_WPA_WPA2_PSK`. Do đó các AP chỉ hỗ trợ WPA3-only hoặc mạng mở (open/no-password) sẽ **không** kết nối được.
*   **Điều kiện tiên quyết**: Caller bắt buộc phải gọi `nvs_flash_init()`, `esp_netif_init()`, và `esp_event_loop_create_default()` trước khi gọi hàm này. (Component tự gọi `esp_netif_create_default_wifi_sta()` bên trong, không cần caller tạo netif STA.)

### 1.2 Kiểm tra trạng thái kết nối
```c
bool svc_network_is_connected(void);
```
*   Trả về `true` nếu thiết bị hiện đang duy trì kết nối mạng vật lý ổn định (đã có IP).

---

## 2. Mục đích (Purpose / Why)
Chịu trách nhiệm quản lý tầng kết nối mạng vật lý thấp nhất (Layer 1/2). Đảm bảo thiết bị luôn được kết nối với Internet thông qua WiFi Station (STA) hoặc 4G LTE (PPPoS) và tự động khôi phục kết nối khi mất sóng.

## 3. Cơ chế cốt lõi (How it works)

### 3.1 Khởi tạo non-blocking (Asynchronous Boot)
Hàm `svc_network_init()` **không chặn**: nó cấu hình và gọi `esp_wifi_start()` rồi trả về `ESP_OK` ngay lập tức, không chờ kết nối hay cấp phát IP. Nhờ đó quá trình kết nối WiFi diễn ra song song với các tác vụ khởi động khác (ví dụ hiệu chuẩn IMU) mà không cần Sync Barrier:
1.  `app_main` khởi tạo nền tảng Netif và Event Loop chung.
2.  `svc_network_init()` đăng ký event handler, khởi động WiFi STA và trả về ngay.
3.  Toàn bộ quá trình kết nối được xử lý **bất đồng bộ** trong `wifi_event_handler`.

### 3.2 Cơ chế tự kết nối lại (Auto-reconnect)
Mỗi khi xảy ra `WIFI_EVENT_STA_DISCONNECTED`, handler lập tức:
1.  Đặt cờ kết nối về `false` và phát `NET_EVT_DISCONNECTED`.
2.  Gọi lại `esp_wifi_connect()` để thử kết nối lại — **không giới hạn số lần thử** nhằm đảm bảo tính sẵn sàng.

> **Lưu ý**: Do retry không giới hạn và mỗi lần ngắt đều phát event, `NET_EVT_DISCONNECTED` có thể được phát **rất thường xuyên** khi sóng yếu/chập chờn. Các consumer cần xử lý debounce/lọc trùng nếu cần.

### 3.3 Cơ chế Auto-Detect 4G LTE (Sniff Bus UART)
Khi khởi động, hệ thống ưu tiên thử khởi tạo mạng di động qua `svc_network_init_cellular()`. Việc phát hiện module dựa trên **hoạt động sớm trên bus UART**, **không** dựa AT-response (vì module boot mất ~30s).
Quy trình:
1. Bật pull-up RX, nghe event queue **tối đa 10s**, đếm `UART_BREAK`/`UART_DATA`. Cần **≥5 event bền** mới kết luận CÓ module.
2. **CÓ module** → tiến hành đồng bộ AT (đợi tối đa 35s). Nếu thấy byte rác PPP, gửi lệnh `+++` thoát mode DATA. Khóa băng tần LTE-only (CFUN=0 → AT+CNMP=38 → CFUN=1 lần đầu, các lần sau chỉ poll `CGATT?` để tăng tốc boot tiết kiệm 20s).
3. **KHÔNG module** → bus im hết 10s → trả `FAIL`, fallback sang `svc_network_init()` kết nối WiFi.
> ⚠️ Tuyệt đối không gửi `+++` khi bus im, vì lệnh này chờ ACK mất 25s, gây treo hệ thống oan nếu không có mạch 4G.

## 4. Đa nhiệm & Tài nguyên (Concurrency & Resources)
* WiFi/PPP hoạt động ngầm (background) do hệ điều hành ESP-IDF tự quản lý task (Ví dụ Task `tiT` của WiFi, hay Task modem của PPPoS).
* Không chiếm CPU khi chờ mạng kết nối. Việc đo tín hiệu/cấu hình đều qua ngắt hoặc Event.

## 5. Luồng giao tiếp (Data Flow & Events)
Dịch vụ thông báo trạng thái cấp phát IP cho các service khác (như `svc_cloud`, `sys_manager`) thông qua Default Event Loop. 

Sử dụng cơ sở sự kiện `NET_EVENT`:
*   `NET_EVT_WIFI_CONNECTED`: Phát ra khi kết nối WiFi thành công và đã nhận IP hợp lệ.
*   `NET_EVT_CELLULAR_CONNECTED`: Phát ra khi kết nối 4G PPPoS thành công và đã nhận IP.
*   `NET_EVT_DISCONNECTED`: Phát ra mỗi lần mất kết nối vật lý (WiFi hoặc 4G), báo hiệu cho hệ thống ngừng truyền telemetry để tiết kiệm pin.
