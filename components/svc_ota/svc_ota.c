#include "svc_ota.h"
#include "sys_manager.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

#define OTA_BUF_SIZE 4096

static const char *TAG = "SVC_OTA";

typedef struct {
    char url[256];
} ota_task_arg_t;

/**
 * @brief Task OTA: tải firmware từ HTTP server, ghi vào OTA partition, reboot.
 *        Chạy trên task riêng để không block event loop MQTT.
 *        Rollback về STATE_NORMAL nếu bất kỳ bước nào thất bại.
 * @param arg Con trỏ tới ota_task_arg_t (được free trong task trước khi thoát).
 */
static void ota_task(void *arg)
{
    ota_task_arg_t *params = (ota_task_arg_t *)arg;

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA update partition found");
        goto fail;
    }
    ESP_LOGI(TAG, "OTA target partition: %s at offset 0x%lx",
             update_partition->label, update_partition->address);

    esp_http_client_config_t http_cfg = {
        .url = params->url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        /// skip_cert_common_name_check cho phép dùng cert self-signed ở dev;
        /// production nên thay bằng cert_pem chỉ định CA cụ thể.
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        goto fail;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        goto fail;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP server returned status %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto fail;
    }
    ESP_LOGI(TAG, "Firmware size: %d bytes", content_length);

    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto fail;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "OTA buffer malloc failed");
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto fail;
    }

    int data_read;
    int total_written = 0;
    while ((data_read = esp_http_client_read(client, buf, OTA_BUF_SIZE)) > 0) {
        err = esp_ota_write(ota_handle, buf, data_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            goto fail;
        }
        total_written += data_read;
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_written == 0) {
        ESP_LOGE(TAG, "No data received from server");
        esp_ota_abort(ota_handle);
        goto fail;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        goto fail;
    }

    ESP_LOGI(TAG, "OTA success! Written %d bytes. Rebooting in 2s...", total_written);
    free(params);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    // (không bao giờ chạy tới đây)

fail:
    ESP_LOGE(TAG, "OTA failed. Rolling back to STATE_NORMAL.");
    sys_manager_set_state(STATE_NORMAL);
    free(params);
    vTaskDelete(NULL);
}

/**
 * @brief Spawn task tải firmware từ URL về và flash vào partition OTA kế tiếp.
 * @param firmware_url URL HTTP/HTTPS trỏ tới file firmware .bin.
 * @return ESP_OK nếu task được tạo thành công.
 */
esp_err_t svc_ota_trigger(const char *firmware_url)
{
    if (firmware_url == NULL || strlen(firmware_url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_task_arg_t *arg = malloc(sizeof(ota_task_arg_t));
    if (arg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(arg->url, firmware_url, sizeof(arg->url) - 1);
    arg->url[sizeof(arg->url) - 1] = '\0';

    /// Stack 8KB: esp_http_client + esp_ota_write cần stack lớn hơn task thông thường.
    BaseType_t created = xTaskCreate(ota_task, "svc_ota_task", 8192, arg, 5, NULL);
    if (created != pdPASS) {
        free(arg);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
