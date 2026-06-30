#ifndef SVC_NETWORK_H
#define SVC_NETWORK_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Khởi tạo dịch vụ mạng WiFi ở chế độ Station (STA) và bắt đầu kết nối.
 * @param ssid Tên mạng WiFi (SSID) cần kết nối.
 * @param pass Mật khẩu WiFi tương ứng với SSID.
 * @return ESP_OK nếu khởi tạo thành công.
 */
esp_err_t svc_network_init(const char *ssid, const char *pass);

/**
 * @brief Kiểm tra trạng thái kết nối mạng hiện tại (WiFi hoặc 4G/PPP).
 * @return true nếu đã kết nối và lấy được địa chỉ IP, ngược lại false.
 */
bool svc_network_is_connected(void);

/**
 * @brief Lấy RSSI của kết nối hiện tại.
 * @return rssi (dBm) nếu dùng WiFi, 0 nếu dùng 4G LTE (đợi làm CMUX sau).
 */
int svc_network_get_rssi(void);

/// Cấu hình phần cứng + APN cho đường mạng 4G LTE (PPPoS qua A7680C).
typedef struct {
    const char *apn;   ///< APN nhà mạng (vd Viettel "v-internet").
    const char *user;  ///< Username APN (để "" nếu không có).
    const char *pass;  ///< Password APN (để "" nếu không có).
    int uart_port;     ///< Cổng UART nối module (vd UART_NUM_2).
    int tx_pin;        ///< Chân TX (ESP -> module).
    int rx_pin;        ///< Chân RX (module -> ESP).
    int rst_pin;       ///< Chân RST điều khiển reset module.
} svc_network_cellular_cfg_t;

/**
 * @brief Khởi tạo đường mạng 4G LTE qua PPPoS (A7680C + esp_modem).
 * @param cfg Con trỏ tới cấu hình phần cứng/APN.
 * @return ESP_OK nếu bring-up thành công tới bước dial PPP; mã lỗi nếu thất bại.
 *
 * Bật nguồn module (PWRKEY), tạo netif PPP + DCE esp_modem, KHÓA LTE-only
 * (AT+CNMP=38) rồi chuyển sang chế độ DATA. Khi nhận IP sẽ phát NET_EVT_CELLULAR_CONNECTED.
 */
esp_err_t svc_network_init_cellular(const svc_network_cellular_cfg_t *cfg);

/**
 * @brief Đặt chu kỳ đo RSSI 4G.
 * @param ms Chu kỳ tính bằng milli-giây. 0 = tắt hẳn. Giá trị khác 0 sẽ bị clamp tối thiểu 60000 ms.
 */
void svc_network_set_rssi_interval_ms(uint32_t ms);

/**
 * @brief Lấy chu kỳ đo RSSI 4G hiện tại.
 * @return Chu kỳ (milli-giây). 0 nếu đang tắt.
 */
uint32_t svc_network_get_rssi_interval_ms(void);

#endif // SVC_NETWORK_H
