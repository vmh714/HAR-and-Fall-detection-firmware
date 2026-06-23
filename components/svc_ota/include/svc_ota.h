#ifndef SVC_OTA_H
#define SVC_OTA_H

#include "esp_err.h"

/**
 * @brief Spawn task tải firmware từ URL về và flash vào partition OTA kế tiếp.
 *        Sau khi flash xong, thiết bị tự động restart với firmware mới.
 *        Nếu thất bại, FSM rollback về STATE_NORMAL và giữ nguyên firmware cũ.
 * @param firmware_url URL HTTP/HTTPS trỏ tới file firmware .bin.
 * @return ESP_OK nếu task được tạo thành công; ESP_ERR_INVALID_ARG nếu URL rỗng;
 *         ESP_ERR_NO_MEM nếu không đủ heap để tạo task.
 */
esp_err_t svc_ota_trigger(const char *firmware_url);

#endif // SVC_OTA_H
