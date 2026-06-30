#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/uart.h"

#include "esp_app_desc.h"
#include "esp_mac.h"

// Component headers
#include "hardware_config.h"
#include "sys_manager.h"
#include "svc_network.h"
#include "svc_cloud.h"
#include "svc_ai.h"
#include "driver/i2c_master.h"
#include "mpu6050.h"
#include "imu_service.h"
#include "drv_battery.h"

static const char *TAG = "MAIN_APP";

// ======================== APP MAIN ========================
void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "    Tên dự án : %s", app_desc->project_name);
    ESP_LOGI(TAG, "    Phiên bản : %s", app_desc->version);
    ESP_LOGI(TAG, "    Ngày Build: %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, "=========================================");

    // 1. Khởi tạo các hệ thống nền tảng của ESP-IDF (NVS, Netif)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());

    // 2. Khởi tạo FSM Trung tâm (Event Loop)
    sys_manager_init();

    // Battery monitor (ADC) — đọc % pin cho telemetry
    drv_battery_init(BATTERY_ADC_GPIO, BATTERY_DIVIDER_RATIO);

    // 3. Khởi tạo các Service thực tế cho Mạng và Cloud
    uint8_t force_wifi = 0;
    nvs_handle_t nh;
    if (nvs_open("config", NVS_READONLY, &nh) == ESP_OK) {
        nvs_get_u8(nh, "net_mode", &force_wifi);
        nvs_close(nh);
    }

    if (force_wifi) {
        ESP_LOGI(TAG, "NVS flag 'net_mode' = 1: Bỏ qua 4G, ép dùng WiFi (Force WiFi)");
        svc_network_init(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
    } else {
        ESP_LOGI(TAG, "Attempting to initialize Network Service (4G LTE / PPPoS) first...");
        svc_network_cellular_cfg_t cell_cfg = {
            .apn = A7680C_APN,
            .user = A7680C_APN_USER,
            .pass = A7680C_APN_PASS,
            .uart_port = UART_PORT,
            .tx_pin = A7680C_TX_PIN,
            .rx_pin = A7680C_RX_PIN,
            .rst_pin = A7680C_RST_PIN,
        };
        
        if (svc_network_init_cellular(&cell_cfg) != ESP_OK) {
            ESP_LOGW(TAG, "4G LTE init failed (module missing). Falling back to WiFi STA...");
            svc_network_init(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
        }
    }

    ESP_LOGI(TAG, "Initializing Cloud Service...");
    /// device_id (khóa topic MQTT) = MAC chip. MAC nằm trong eFuse → ổn định qua
    /// erase-flash/OTA, mỗi board tự duy nhất, không cần provision. Backend nhận MAC
    /// rồi tự sinh id ngữ nghĩa (esp32_eldercare_NN) phía server.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_id[16];
    snprintf(mac_id, sizeof(mac_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device topic key (MAC): %s", mac_id);
    svc_cloud_init(CONFIG_MQTT_BROKER_URI, mac_id, CONFIG_MQTT_USERNAME, CONFIG_MQTT_PASSWORD);

    ESP_LOGI(TAG, "Initializing I2C and MPU6050...");
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));
    mpu6050_init(bus_handle);

    // IMU Service: config + calibrate + PCNT + processing task (tất cả bên trong)
    ESP_LOGI(TAG, "Initializing IMU Service...");
    imu_service_init(MPU6050_INT_PIN);
    imu_service_register_batch_callback(svc_cloud_enqueue_imu_batch);

    // 4. Khởi tạo AI Service
    ESP_LOGI(TAG, "Initializing AI Service...");
    svc_ai_init();

    ESP_LOGI(TAG, "System Skeleton Initialized. Main thread yields.");
}

