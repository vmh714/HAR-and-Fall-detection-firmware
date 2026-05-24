#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"

// Component headers
#include "hardware_config.h"
#include "sys_manager.h"
#include "svc_network.h"
#include "svc_cloud.h"
#include "driver/i2c_master.h"
#include "mpu6050.h"
#include "imu_service.h"

static const char *TAG = "MAIN_APP";

// ======================== PHASE 2 TEST CODE ========================
// Task giả lập các lệnh điều khiển từ xa sau khi đã kết nối
// static void test_fsm_task(void *pvParameters) {
//     ESP_LOGW("TEST_FSM", "Waiting 20 seconds before simulating START STREAM command...");
//     vTaskDelay(pdMS_TO_TICKS(20000));
    
//     ESP_LOGW("TEST_FSM", "Simulating Cloud Start Stream Command...");
//     esp_event_post(CLOUD_EVENT, CLOUD_CMD_START_STREAM, NULL, 0, portMAX_DELAY);
    
//     vTaskDelay(pdMS_TO_TICKS(10000));
    
//     ESP_LOGW("TEST_FSM", "Simulating Cloud Stop Stream Command...");
//     esp_event_post(CLOUD_EVENT, CLOUD_CMD_STOP_STREAM, NULL, 0, portMAX_DELAY);

//     vTaskDelete(NULL);
// }
// ====================================================================

// ======================== APP MAIN ========================
void app_main(void)
{
    ESP_LOGI(TAG, "==== ESP32-S3 ELDERCARE FIRMWARE ====");

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

    // 3. Khởi tạo các Service thực tế cho Mạng và Cloud
    ESP_LOGI(TAG, "Initializing Network Service...");
    svc_network_init(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);

    ESP_LOGI(TAG, "Initializing Cloud Service...");
    svc_cloud_init(CONFIG_MQTT_BROKER_URI, CONFIG_DEVICE_ID, CONFIG_MQTT_USERNAME, CONFIG_MQTT_PASSWORD);

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

    mpu6050_config_t my_cfg = {
        .accel_fs = ACCEL_FS_8G,
        .gyro_fs = GYRO_FS_500DPS,
        .dlpf_cfg = DLPF_CFG_21HZ,
        .sample_rate_hz = 100, // 100Hz
        .pwr_cfg = { .temp_disable = true },
        .int_cfg = {
            .data_ready_en = true,
            .active_low = true,
            .latch_en = false,
        },
        .fifo_cfg = {
            .fifo_enable = true,
            .accel_fifo_en = true,
            .gyro_fifo_en = true,
            .temp_fifo_en = false,
        }
    };
    mpu6050_config(&my_cfg);
    mpu6050_calibrate_gyro();

    ESP_LOGI(TAG, "Initializing IMU Service...");
    imu_service_init(MPU6050_INT_PIN);
    imu_service_register_batch_callback(svc_cloud_enqueue_imu_batch);

    ESP_LOGI(TAG, "System Skeleton Initialized. Main thread yields.");

    // Chạy Task test giả lập lệnh điều khiển FSM
    //xTaskCreate(test_fsm_task, "test_fsm_task", 2048, NULL, 5, NULL);
}
