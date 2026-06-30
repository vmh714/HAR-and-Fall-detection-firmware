# Component: svc_ota (Over-The-Air Update Service)

> **Cập nhật cuối (Timestamp):** 2026-06-30
> **Trạng thái:** Hoạt động ổn định

---

## 1. PUBLIC API

Các hàm công bố trong [svc_ota.h](include/svc_ota.h):

### 1.1 Kích hoạt tiến trình cập nhật OTA
```c
esp_err_t svc_ota_trigger(const char *firmware_url);
```
*   **Mô tả**: Hàm nhận vào URL của file firmware (`.bin`) từ server. Hàm không chạy quá trình tải xuống trực tiếp trên luồng gọi mà **sinh ra một FreeRTOS task riêng biệt** (`svc_ota_task`) để thực thi.
*   **Đầu ra**: Trả về `ESP_OK` nếu task OTA được tạo thành công, `ESP_ERR_INVALID_ARG` nếu URL rỗng, hoặc `ESP_ERR_NO_MEM` nếu hết bộ nhớ cấp phát cho task.

---

## 2. Mục đích (Purpose / Why)
Thực hiện quá trình nạp chương trình cơ sở (firmware) từ xa thông qua mạng (WiFi hoặc 4G). Dịch vụ giúp thiết bị cập nhật tính năng mới hoặc vá lỗi mà không cần thu hồi phần cứng hay cắm cáp USB.

## 3. Cơ chế cốt lõi (How it works)
Khi được kích hoạt, task `svc_ota_task` thực thi tuần tự các bước:
1. **Tìm vùng nhớ trống**: Gọi `esp_ota_get_next_update_partition()` để tìm phân vùng OTA chưa được sử dụng (ví dụ đang ở `ota_0` thì nó sẽ ghi đè lên `ota_1`).
2. **Khởi tạo HTTP Client**: Thiết lập kết nối HTTP/HTTPS để tải file `firmware_url`. Mặc định được cấu hình `skip_cert_common_name_check = true` để chấp nhận chứng chỉ tự ký (self-signed) trong môi trường dev.
3. **Ghi luồng (Stream Write)**: Tải từng khối (chunk) dung lượng `4096 bytes` từ server và ghi trực tiếp vào Flash thông qua `esp_ota_write()`. Việc ghi tuần tự (OTA_WITH_SEQUENTIAL_WRITES) giúp tiết kiệm cực kỳ nhiều RAM.
4. **Xác thực và Khởi động lại**: Kết thúc quá trình ghi, nếu thành công (`esp_ota_end`), dịch vụ sẽ chuyển phân vùng boot sang phân vùng vừa ghi (`esp_ota_set_boot_partition`) và tự động gọi `esp_restart()` sau 2 giây.
5. **Cơ chế Fallback (Thất bại)**: Nếu quá trình tải HTTP bị rớt mạng, hoặc lỗi ghi Flash, dịch vụ sẽ đóng kết nối, hủy OTA (`esp_ota_abort`), và đẩy hệ thống (thông qua `sys_manager_set_state()`) quay về trạng thái `STATE_NORMAL` để tiếp tục hoạt động cũ.

## 4. Đa nhiệm & Tài nguyên (Concurrency & Resources)
* Dịch vụ sinh ra Task riêng `svc_ota_task` với stack size `8192 bytes` (do thư viện `esp_http_client` và HTTPS TLS tốn khá nhiều RAM cho quá trình bắt tay SSL) và mức ưu tiên `Priority = 5`.
* Việc cấp phát task riêng cho phép Event Loop chung của hệ thống không bị block trong quá trình tải xuống kéo dài hàng phút.

## 5. Luồng giao tiếp (Data Flow)
* **Kích hoạt**: `svc_ota_trigger` thường được gọi bởi `svc_cloud` khi nhận được lệnh `CLOUD_CMD_OTA_UPDATE` từ server qua MQTT.
* **Tương tác FSM**: `sys_manager` đã được chuyển sang trạng thái `STATE_OTA` trước đó. Nếu OTA thất bại, `svc_ota` giao tiếp trực tiếp với `sys_manager` để đặt trạng thái ngược về `STATE_NORMAL`.
