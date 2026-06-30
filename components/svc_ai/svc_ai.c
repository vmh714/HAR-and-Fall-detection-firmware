#include "svc_ai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "imu_service.h"
#include "sys_manager.h"
#include "tflite_wrapper.h"
#include <string.h>
#include "nvs.h"
#include "esp_timer.h"

static const char* TAG = "SVC_AI";

typedef enum {
    FALL_FSM_NORMAL,
    FALL_FSM_CONFIRMING
} fall_fsm_state_t;

static fall_fsm_state_t s_fall_state = FALL_FSM_NORMAL;
static int64_t s_confirm_start_us = 0;
static float s_trigger_conf = 0.0f;
static uint32_t s_hits = 0;
static uint32_t s_total = 0;
static uint32_t s_confirm_window_ms = 4000; // Mặc định 4s

/// Message qua Queue chỉ chứa con trỏ tới window (nhằm tránh copy dữ liệu lớn
/// nhiều lần trong lúc luân chuyển)
typedef struct
{
    const imu_window_t* window;
} ai_window_msg_t;

static QueueHandle_t s_ai_queue = NULL;
static char s_latest_pred_str[16] = "Unknown";
static float s_latest_pred_conf = 0.0f;

/**
 * @brief Lấy chuỗi text của kết quả dự đoán AI gần nhất.
 * @return Con trỏ tới chuỗi tĩnh (ví dụ "Walk", "Run", "Idle", "Fall").
 */
const char* svc_ai_get_latest_prediction(void)
{
    return s_latest_pred_str;
}

/**
 * @brief Lấy độ tự tin (confidence) của kết quả dự đoán AI gần nhất.
 * @return Giá trị float trong khoảng 0.0 đến 1.0.
 */
float svc_ai_get_latest_confidence(void)
{
    return s_latest_pred_conf;
}

/// Buffer phẳng hóa để chứa data interleaved truyền vào model TFLite.
/// Cấu trúc của Inference Input là một mảng 1D xen kẽ: ax0, ay0, az0, gx0, gy0,
/// gz0, ax1, ay1... Kích thước: 200 mẫu * 6 trục = 1200 phần tử float.
static float s_inference_buffer[IMU_WINDOW_SIZE * 6];

/**
 * @brief Task FreeRTOS chạy vòng lặp inference AI.
 *
 * Khởi tạo TFLite Micro một lần, sau đó lặp vô hạn: chờ window từ queue, phẳng
 * hóa dữ liệu SoA của Ring Buffer thành mảng 1D interleaved, chạy inference và
 * xử lý kết quả (cập nhật trạng thái, xác định tư thế, phát event khi phát hiện
 * ngã).
 *
 * @param pvParameters Tham số task FreeRTOS (không sử dụng).
 */
static void svc_ai_task(void* pvParameters)
{
    ESP_LOGI(TAG, "AI Task started. Initializing TFLite Micro...");

    /// Khởi tạo TFLite Micro (Allocate PSRAM Tensor Arena, load model)
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
            /// Dữ liệu từ svc_imu được chuẩn hóa và lưu trữ rời rạc (SoA -
            /// Structure of Arrays) theo kiểu: win->ax[], win->ay[]... Ngoài ra,
            /// đây là Ring Buffer. Mẫu cũ nhất nằm ở vị trí `head`, mẫu mới nhất
            /// nằm ở `head - 1`. Do đó chúng ta phải duyệt vòng tròn từ `head`
            /// tới `head + 200` và nhồi xen kẽ vào buffer.
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
            /// tflite_run_inference_with_data sẽ nhận float interleaved buffer
            /// này. Hàm sẽ chịu trách nhiệm Quantization (float -> int8),
            /// Invoke() và Thresholding.
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
                // (strncpy + ép null terminator để tránh tràn buffer)
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

                float current_roll = 0.0f;
                imu_service_get_latest_roll(&current_roll);
                
                // Trục tọa độ: 0 là nằm ngang (nằm sấp/ngửa), 90 hoặc -90 là đứng thẳng.
                // Ngưỡng 45 độ dùng để chia cắt: <45 là nằm, >45 là đứng/ngồi.
                bool is_flat = (current_roll > -45.0f && current_roll < 45.0f);
                bool is_stand_sit = !is_flat;

                // Bộ đếm theo dõi xem thiết bị có từng ở trạng thái Đứng/Ngồi trong 3s qua không.
                static int s_not_flat_ticks = 0;
                if (!is_flat) {
                    s_not_flat_ticks = 6; // 6 * 0.5s = 3 seconds
                } else if (s_not_flat_ticks > 0) {
                    s_not_flat_ticks--;
                }

                if (result.predicted_class == AI_CLASS_IDLE)
                {
                    ESP_LOGI(TAG, "Posture: %s (Roll: %.1f deg)",
                             is_stand_sit ? "Stand/Sit" : "Lying Down",
                             current_roll);
                }

                // Fall Confirmation FSM
                if (s_fall_state == FALL_FSM_NORMAL)
                {
                    if (result.predicted_class == AI_CLASS_FALL)
                    {
                        // Lọc nhiễu Edge Case (Báo động giả khi vứt máy trên bàn):
                        // Nếu thiết bị đã nằm bẹp hoàn toàn trong suốt >3 giây qua,
                        // thì 100% không thể là một cú ngã thật sự (vì ngã phải bắt đầu
                        // từ tư thế đứng/ngồi hoặc ít nhất là lộn vòng).
                        if (s_not_flat_ticks == 0) {
                            ESP_LOGW(TAG, ">>> ML Trigger REJECTED: Device was completely flat for >3s (Off-body/On-desk). False alarm blocked.");
                        } else {
                        s_fall_state = FALL_FSM_CONFIRMING;
                        s_confirm_start_us = esp_timer_get_time();
                        s_trigger_conf = result.max_prob;
                        s_hits = 0;
                        s_total = 0;

                        /// Giữ link MQTT suốt cửa sổ xác nhận + 5s biên để alert kịp publish.
                        sys_manager_bump_comms_critical(s_confirm_window_ms + 5000);

                        ESP_LOGW(TAG, ">>> ML Trigger: Fall class detected. Confidence: %.2f. Starting confirmation FSM (Window: %lu ms)...",
                                 s_trigger_conf, (unsigned long)s_confirm_window_ms);

                        // Lấy thông tin cú va chạm (impact) gần nhất để log / boost
                        imu_impact_info_t impact;
                        imu_service_get_last_impact(&impact);
                        int64_t now_us = esp_timer_get_time();
                        if (now_us - impact.ts_us < 1500000)
                        {
                            ESP_LOGI(TAG, "[CONFIRMATION BOOST] Recent impact found: peak_g=%.2fg, had_freefall=%d, age=%lld ms",
                                     impact.peak_g, impact.had_freefall, (now_us - impact.ts_us) / 1000);
                        }
                        else
                        {
                            ESP_LOGW(TAG, "[CONFIRMATION BOOST] No recent impact found within 1.5s (last was %lld ms ago)", (now_us - impact.ts_us) / 1000);
                        }
                        }
                    }
                }
                else if (s_fall_state == FALL_FSM_CONFIRMING)
                {
                    bool is_lying = !is_stand_sit;
                    bool is_ok = (result.predicted_class == AI_CLASS_IDLE) && is_lying;

                    // ABORT nếu class thuộc {WALK, RUN} và roll upright (không nằm)
                    if ((result.predicted_class == AI_CLASS_WALK || result.predicted_class == AI_CLASS_RUN) && is_stand_sit)
                    {
                        ESP_LOGW(TAG, ">>> Confirmation ABORTED: locomotion class %s detected with upright posture.", pred_name);
                        s_fall_state = FALL_FSM_NORMAL;
                    }
                    else
                    {
                        if (result.predicted_class != AI_CLASS_TRANSITION)
                        {
                            s_total++;
                            if (is_ok)
                            {
                                s_hits++;
                            }
                        }

                        int64_t elapsed_ms = (esp_timer_get_time() - s_confirm_start_us) / 1000;
                        ESP_LOGI(TAG, "Confirming: elapsed=%lld ms, hits/total=%lu/%lu, class=%s, roll=%.1f",
                                 elapsed_ms, (unsigned long)s_hits, (unsigned long)s_total, pred_name, current_roll);

                        if (elapsed_ms >= s_confirm_window_ms)
                        {
                            float ratio = s_total > 0 ? (float)s_hits / s_total : 0.0f;
                            if (ratio >= 0.6f)
                            {
                                ESP_LOGW(TAG, ">>> WARNING: FALL CONFIRMED! Ratio: %.2f (hits/total: %lu/%lu). Posting event...",
                                         ratio, (unsigned long)s_hits, (unsigned long)s_total);
                                s_latest_pred_conf = s_trigger_conf;
                                esp_event_post(AI_EVENT, AI_EVT_FALL_DETECTED, NULL, 0, portMAX_DELAY);
                            }
                            else
                            {
                                ESP_LOGI(TAG, ">>> Confirmation ABORTED: ratio %.2f < 0.60 (hits/total: %lu/%lu)",
                                         ratio, (unsigned long)s_hits, (unsigned long)s_total);
                            }
                            s_fall_state = FALL_FSM_NORMAL;
                        }
                    }
                }
                ESP_LOGI(TAG, "---------------------------");
            }
        }
    }
}

/**
 * @brief Khởi tạo Service AI: tạo queue nhận window và tạo task inference.
 *
 * Tạo queue chứa tối đa 2 message (con trỏ window), sau đó tạo task FreeRTOS
 * svc_ai_task để chạy inference. tflite_init() được gọi bên trong task.
 *
 * @return ESP_OK nếu khởi tạo thành công, ESP_FAIL nếu tạo queue/task thất bại.
 */
esp_err_t svc_ai_init(void)
{
    s_ai_queue = xQueueCreate(2, sizeof(ai_window_msg_t));
    if (!s_ai_queue)
    {
        ESP_LOGE(TAG, "Failed to create AI queue");
        return ESP_FAIL;
    }

    nvs_handle_t my_handle;
    uint32_t val = 4000;
    if (nvs_open("config", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_u32(my_handle, "fall_cf", &val);
        nvs_close(my_handle);
    }
    s_confirm_window_ms = val;
    ESP_LOGI(TAG, "Loaded fall confirm window from NVS: %lu ms", (unsigned long)s_confirm_window_ms);

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

/**
 * @brief Đẩy window IMU vào queue để task AI xử lý không đồng bộ.
 *
 * Được gọi qua callback từ svc_imu khi có đủ 1 sliding window. Chỉ chuyển con
 * trỏ window vào queue rồi trả về ngay, không xử lý inference tại đây.
 *
 * @param window Con trỏ tới struct imu_window_t chứa Ring Buffer dữ liệu IMU.
 */
void svc_ai_process_window(const imu_window_t* window)
{
    if (s_ai_queue == NULL || window == NULL)
        return;

    ai_window_msg_t msg = {.window = window};

    /// Đẩy pointer vào queue để AI Task lấy ra xử lý.
    /// Timeout 0 để không block luồng gọi (ở đây là luồng của svc_imu)
    xQueueSend(s_ai_queue, &msg, 0);
}

void svc_ai_set_confirm_window_ms(uint32_t ms)
{
    s_confirm_window_ms = ms;
    ESP_LOGI(TAG, "Set fall confirm window to: %lu ms", (unsigned long)s_confirm_window_ms);
}

uint32_t svc_ai_get_confirm_window_ms(void)
{
    return s_confirm_window_ms;
}
