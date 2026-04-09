#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

// Component headers
#include "mpu6050.h"
#include "imu_service.h"
#include "wifi_mqtt_service.h"

static const char *TAG = "MAIN_APP";

// ======================== CẤU HÌNH PHẦN CỨNG ========================
#define IMU_INT_GPIO GPIO_NUM_11
#define I2C_PORT I2C_NUM_0
#define I2C_SCL GPIO_NUM_9
#define I2C_SDA GPIO_NUM_10

// ======================== CẤU HÌNH MQTT ========================
#define MQTT_PUBLISH_TOPIC "test_topic/imu/mpu6050_data/"
#define MQTT_PUBLISH_INTERVAL 1000 // ms (1 Hz)

// ======================== BOOT SYNC (§3.5 instructions.md) ========================
static EventGroupHandle_t s_boot_sync;
#define IMU_READY_BIT  BIT0
#define NET_READY_BIT  BIT1

// ======================== TASK PUBLISH IMU DATA ========================
static void mqtt_imu_publish_task(void *pvParameters)
{
    float roll, pitch;
    mpu6050_data_t raw;
    char payload[300];
    const char *device_id = wifi_mqtt_get_device_id();

    ESP_LOGI(TAG, "MQTT publish task started. Topic: %s | Rate: %d ms",
             MQTT_PUBLISH_TOPIC, MQTT_PUBLISH_INTERVAL);

    while (1)
    {
        if (wifi_mqtt_is_connected())
        {
            // Lấy góc đã lọc Kalman
            imu_service_get_latest_angles(&roll, &pitch);

            // Lấy raw data 6 trục
            raw = mpu6050_read();

            // Tạo JSON payload
            snprintf(payload, sizeof(payload),
                     "{\"device_id\":\"%s\","
                     "\"roll\":%.2f,\"pitch\":%.2f,"
                     "\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
                     "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f}",
                     device_id,
                     roll, pitch,
                     raw.ax, raw.ay, raw.az,
                     raw.gx, raw.gy, raw.gz);

            ESP_LOGI("RAW", "Ax: %.2f | Ay: %.2f | Az: %.2f | Gx: %.2f | Gy: %.2f | Gz: %.2f", raw.ax, raw.ay, raw.az, raw.gx, raw.gy, raw.gz);
            
            int msg_id = wifi_mqtt_publish(MQTT_PUBLISH_TOPIC, payload, 1, 0);
            if (msg_id >= 0)
            {
                ESP_LOGI(TAG, "Published [%d]: R:%.1f P:%.1f", msg_id, roll, pitch);
            }
            else
            {
                ESP_LOGW(TAG, "Publish failed!");
            }
        }
        else
        {
            ESP_LOGW(TAG, "MQTT not connected, skipping publish...");
        }

        vTaskDelay(pdMS_TO_TICKS(MQTT_PUBLISH_INTERVAL));
    }
}

// ======================== WIFI BOOT TASK (Parallel) ========================
// Task tạm thời chạy song song với IMU init, tự hủy sau khi hoàn thành.
static void wifi_boot_task(void *pvParameters)
{
    wifi_mqtt_config_t *cfg = (wifi_mqtt_config_t *)pvParameters;

    ESP_LOGI(TAG, "[PARALLEL] WiFi+MQTT init starting...");
    esp_err_t ret = wifi_mqtt_service_init(cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi+MQTT init failed: %s", esp_err_to_name(ret));
        // Vẫn set bit để app_main không block mãi — xử lý lỗi ở stage 3
    }

    ESP_LOGI(TAG, "[PARALLEL] WiFi+MQTT init done! Setting NET_READY_BIT.");
    xEventGroupSetBits(s_boot_sync, NET_READY_BIT);

    // Task tạm — tự hủy sau khi hoàn thành sứ mệnh
    vTaskDelete(NULL);
}

// ======================== APP MAIN ========================
void app_main(void)
{
    // PRE-INIT: System Singletons (dùng chung, < 10ms)  
    // Phải chạy TRƯỚC khi spawn bất kỳ task parallel nào 
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_boot_sync = xEventGroupCreate();
    ESP_LOGI(TAG, "System singletons initialized. Starting parallel boot...");


    // PARALLEL BOOT: IMU (foreground) ∥ WiFi (background) 


    // --- Nhánh WiFi: Spawn task chạy nền ---
    static wifi_mqtt_config_t net_cfg = {
        .wifi_ssid = "MD",
        .wifi_pass = "11111111",
        .mqtt_broker_uri = "mqtt://mqtt.toolhub.app",
        .mqtt_username = "hungvm",
        .mqtt_password = "20225198",
        .mqtt_client_id = "ESP32S3_01",
    };
    xTaskCreate(wifi_boot_task, "wifi_boot", 4096, &net_cfg, 5, NULL);

    // --- Nhánh IMU: Chạy trực tiếp trong app_main ---
    ESP_LOGI(TAG, "[PARALLEL] IMU init starting...");

    // 1. Khởi tạo I2C Bus
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    // 2. Khởi tạo MPU6050
    mpu6050_init(bus_handle);

    // 3. Cấu hình chi tiết (FIFO + Interrupt)
    mpu6050_config_t imu_cfg = {
        .accel_fs = ACCEL_FS_8G,
        .gyro_fs = GYRO_FS_500DPS,
        .dlpf_cfg = DLPF_CFG_21HZ,
        .sample_rate_hz = 100,

        .pwr_cfg = {
            .temp_disable = true,
        },

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
        },
    };
    mpu6050_config(&imu_cfg);

    // 4. Calibrate Gyro (~6s, thiết bị phải đứng yên)
    mpu6050_calibrate_gyro();

    ESP_LOGI(TAG, "[PARALLEL] IMU hardware ready! Setting IMU_READY_BIT.");
    xEventGroupSetBits(s_boot_sync, IMU_READY_BIT);

    //   SYNC BARRIER: Chờ cả hai nhánh hoàn tất             
    ESP_LOGI(TAG, "Waiting for both IMU and WiFi to be ready...");
    xEventGroupWaitBits(s_boot_sync,
                        IMU_READY_BIT | NET_READY_BIT,
                        pdFALSE,    // Không xóa bits sau khi đọc
                        pdTRUE,     // Chờ TẤT CẢ bits (AND logic)
                        portMAX_DELAY);

    //   STAGE 3: DATA PIPELINE (Cả IMU + WiFi đã sẵn sàng) 

    ESP_LOGI(TAG, "All systems ready! Starting data pipeline...");

    // 3.1 Reset FIFO — xóa rác tích lũy trong lúc chờ WiFi kết nối
    mpu6050_reset_fifo();

    // 3.2 Chạy IMU Service (tạo Task + gắn ISR → bắt đầu fill sliding window)
    imu_service_init(IMU_INT_GPIO);

    // 3.3 Tạo Task Publish MQTT
    xTaskCreate(mqtt_imu_publish_task, "mqtt_pub", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System operational! Data pipeline running.");
}
