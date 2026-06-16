#include "svc_ai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "imu_service.h"
#include "sys_manager.h"
#include "tflite_wrapper.h"
#include <string.h>
static const char* TAG = "SVC_AI";

// Message qua Queue chỉ chứa con trỏ tới window (nhằm tránh copy dữ liệu lớn
// nhiều lần trong lúc luân chuyển)
typedef struct
{
    const imu_window_t* window;
} ai_window_msg_t;

static QueueHandle_t s_ai_queue = NULL;
static char s_latest_pred_str[16] = "Unknown";
static float s_latest_pred_conf = 0.0f;

const char* svc_ai_get_latest_prediction(void)
{
    return s_latest_pred_str;
}

float svc_ai_get_latest_confidence(void)
{
    return s_latest_pred_conf;
}

// Buffer phẳng hóa để chứa data interleaved truyền vào model TFLite.
// Cấu trúc của Inference Input là một mảng 1D xen kẽ: ax0, ay0, az0, gx0, gy0,
// gz0, ax1, ay1... Kích thước: 200 mẫu * 6 trục = 1200 phần tử float.
static float s_inference_buffer[IMU_WINDOW_SIZE * 6];

static void svc_ai_task(void* pvParameters)
{
    ESP_LOGI(TAG, "AI Task started. Initializing TFLite Micro...");

    // Khởi tạo TFLite Micro (Allocate PSRAM Tensor Arena, load model)
    if (tflite_init() != 0)
    {
        ESP_LOGE(TAG, "TFLite Initialization failed! AI Task stopped.");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TFLite initialized successfully.");

    ai_window_msg_t msg;
    while (1)
    {
        if (xQueueReceive(s_ai_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            const imu_window_t* win = msg.window;
            uint16_t head = win->head;

            // --- BƯỚC 1: Flatten & Interleave Data ---
            // Dữ liệu từ svc_imu được chuẩn hóa và lưu trữ rời rạc (SoA -
            // Structure of Arrays) theo kiểu: win->ax[], win->ay[]... Ngoài ra,
            // đây là Ring Buffer. Mẫu cũ nhất nằm ở vị trí `head`, mẫu mới nhất
            // nằm ở `head - 1`. Do đó chúng ta phải duyệt vòng tròn từ `head`
            // tới `head + 200` và nhồi xen kẽ vào buffer.
            int dst_idx = 0;
            for (int i = 0; i < IMU_WINDOW_SIZE; i++)
            {
                int src_idx = (head + i) % IMU_WINDOW_SIZE;

                s_inference_buffer[dst_idx++] = win->ax[src_idx];
                s_inference_buffer[dst_idx++] = win->ay[src_idx];
                s_inference_buffer[dst_idx++] = win->az[src_idx];
                s_inference_buffer[dst_idx++] = win->gx[src_idx];
                s_inference_buffer[dst_idx++] = win->gy[src_idx];
                s_inference_buffer[dst_idx++] = win->gz[src_idx];
            }

            // --- BƯỚC 2: Thực thi Inference ---
            // tflite_run_inference_with_data sẽ nhận float interleaved buffer
            // này. Hàm sẽ chịu trách nhiệm Quantization (float -> int8),
            // Invoke() và Thresholding.
            size_t num_bytes = sizeof(s_inference_buffer);
            ESP_LOGD(TAG, "Running inference on window of %d samples...",
                     IMU_WINDOW_SIZE);
            ai_inference_result_t result =
                tflite_run_inference_with_data(s_inference_buffer, num_bytes);

            // --- BƯỚC 3: Xử lý và Log kết quả (Business Logic) ---
            if (result.is_valid)
            {
                const char* class_names[] = {"Walk", "Run", "Idle", "Trans",
                                             "Fall"};
                const char* pred_name =
                    (result.predicted_class < AI_CLASS_UNKNOWN)
                        ? class_names[result.predicted_class]
                        : "Unknown";

                // Cập nhật trạng thái mới nhất cho các module khác gọi
                strncpy(s_latest_pred_str, pred_name,
                        sizeof(s_latest_pred_str) - 1);
                s_latest_pred_str[sizeof(s_latest_pred_str) - 1] = '\0';
                s_latest_pred_conf = result.max_prob;

                ESP_LOGI(TAG, "--- AI INFERENCE RESULT ---");
                ESP_LOGI(
                    TAG,
                    "Time: %.2f ms | Pred: %s (%.1f%%) | Fall Prob: %.1f%%",
                    result.inference_time_us / 1000.0, pred_name,
                    result.max_prob * 100.0, result.fall_prob * 100.0);

                // Nếu trạng thái là IDLE, dùng góc Pitch từ Kalman filter để
                // xác định tư thế (Posture)
                if (result.predicted_class == AI_CLASS_IDLE)
                {
                    float current_pitch = 0.0f;
                    imu_service_get_latest_pitch(&current_pitch);

                    // Logic cơ bản: Pitch nằm trong khoảng [-45, 45] độ ->
                    // Đứng/Ngồi, ngoài ra là Nằm
                    bool is_stand_sit =
                        (current_pitch > -45.0f && current_pitch < 45.0f);
                    ESP_LOGI(TAG, "Posture: %s (Pitch: %.1f deg)",
                             is_stand_sit ? "Stand/Sit" : "Lying Down",
                             current_pitch);
                }

                if (result.predicted_class == AI_CLASS_FALL)
                {
                    ESP_LOGW(TAG, ">>> WARNING: FALL DETECTED! <<<");
                    // Post sự kiện AI_EVT_FALL_DETECTED lên Event Loop để
                    // svc_cloud publish MQTT
                    esp_event_post(AI_EVENT, AI_EVT_FALL_DETECTED, NULL, 0,
                                   portMAX_DELAY);
                }
                ESP_LOGI(TAG, "---------------------------");
            }
        }
    }
}

esp_err_t svc_ai_init(void)
{
    s_ai_queue = xQueueCreate(2, sizeof(ai_window_msg_t));
    if (!s_ai_queue)
    {
        ESP_LOGE(TAG, "Failed to create AI queue");
        return ESP_FAIL;
    }

    BaseType_t ret =
        xTaskCreate(svc_ai_task, "svc_ai_task", 6144, NULL, 6, NULL);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create AI task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Service AI initialized");
    return ESP_OK;
}

void svc_ai_process_window(const imu_window_t* window)
{
    if (s_ai_queue == NULL || window == NULL)
        return;

    ai_window_msg_t msg = {.window = window};

    // Đẩy pointer vào queue để AI Task lấy ra xử lý
    // Timeout 0 để không block luồng gọi (ở đây là luồng của svc_imu)
    xQueueSend(s_ai_queue, &msg, 0);
}
