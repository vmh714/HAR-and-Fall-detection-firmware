#include "tflite_wrapper.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h" // Thêm thư viện để cấp phát PSRAM
//#include <math.h>

#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "model_data.h"

static const char* TAG = "TFLiteWrapper";

namespace {
    const tflite::Model* model = nullptr;
    tflite::MicroInterpreter* interpreter = nullptr;
    TfLiteTensor* input = nullptr;
    TfLiteTensor* output = nullptr;

    // Tăng kích thước lên 1MB
    constexpr int kTensorArenaSize = 100 * 1024;
    
    //uint8_t tensor_arena[kTensorArenaSize];
    //Đổi từ mảng tĩnh sang con trỏ để cấp phát động
    uint8_t* tensor_arena = nullptr;
}  // namespace

int tflite_init(void) {
    tflite::InitializeTarget();

    //1. Cấp phát Tensor Arena vào PSRAM
    tensor_arena = (uint8_t*)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM);
    if (tensor_arena == nullptr) {
        ESP_LOGE(TAG, "Lỗi: Không thể cấp phát %d bytes trên PSRAM!", kTensorArenaSize);
        return -1;
    }
    ESP_LOGI(TAG, "Đã cấp phát thành công %d bytes trên PSRAM.", kTensorArenaSize);

    // 2. Load mô hình
    model = tflite::GetModel(g_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model provided is schema version %d not equal to supported version %d.",
                 (int)model->version(), (int)TFLITE_SCHEMA_VERSION);
        return -1;
    }

    // 3. Đăng ký các toán tử cần thiết cho model v25 (đã được trích xuất)
    static tflite::MicroMutableOpResolver<15> resolver;
    resolver.AddAdd();
    resolver.AddConcatenation();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D(); // Thêm op này để sửa lỗi DEPTHWISE_CONV_2D
    resolver.AddExpandDims();
    resolver.AddFullyConnected();
    resolver.AddLogistic();
    resolver.AddMean();
    resolver.AddMul();
    resolver.AddPack();
    resolver.AddReduceMax();
    resolver.AddReshape();
    resolver.AddShape();
    resolver.AddSoftmax();
    resolver.AddStridedSlice();

    // 4. Build Interpreter
    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize);
    interpreter = &static_interpreter;

    // 5. Cấp phát Tensor vào vùng nhớ đã tạo
    TfLiteStatus allocate_status = interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors() failed");
        return -1;
    }

    // In ra lượng RAM thực tế yêu cầu
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "TFLITE ARENA CALCULATION RESULTS:");
    ESP_LOGI(TAG, "Total Arena Size Configured: %d bytes (PSRAM)", kTensorArenaSize);
    ESP_LOGI(TAG, "Actual Arena Used: %d bytes", (int)interpreter->arena_used_bytes());
    ESP_LOGI(TAG, "================================================");

    input = interpreter->input(0);
    output = interpreter->output(0);
    
    ESP_LOGI(TAG, "Model initialized successfully!");
    ESP_LOGI(TAG, "Input shape: [%d, %d, %d]", input->dims->data[0], input->dims->data[1], input->dims->data[2]);
    return 0;
}

void tflite_run_inference(void) {
    if (interpreter == nullptr || input == nullptr || output == nullptr) {
        ESP_LOGE(TAG, "Interpreter not initialized!");
        return;
    }

    if (input->type == kTfLiteFloat32) {
        for (int i = 0; i < input->bytes / sizeof(float); ++i) {
            input->data.f[i] = 0.0f;
        }
    } else if (input->type == kTfLiteInt8) {
        for (int i = 0; i < input->bytes; ++i) {
            input->data.int8[i] = 0; 
        }
    }

    int64_t start_time = esp_timer_get_time();
    TfLiteStatus invoke_status = interpreter->Invoke();
    int64_t end_time = esp_timer_get_time();
    int64_t inference_time_us = end_time - start_time;

    if (invoke_status != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed!");
        return;
    }

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "INFERENCE PERFORMANCE:");
    ESP_LOGI(TAG, "Inference Time: %lld microseconds (%.2f ms)", inference_time_us, inference_time_us / 1000.0);
    ESP_LOGI(TAG, "================================================");
}

int get_input_bytes(void) {
    if (input != nullptr) {
        // Luôn trả về số byte của mảng FLOAT32 (để tương thích với data thật từ IMU/Python)
        int elements = 1;
        for (int i = 0; i < input->dims->size; ++i) {
            elements *= input->dims->data[i];
        }
        return elements * sizeof(float);
    }
    return 0;
}

ai_inference_result_t tflite_run_inference_with_data(float* rx_data, size_t num_bytes) {
    ai_inference_result_t result = {0};
    result.is_valid = false;

    if (interpreter == nullptr || input == nullptr || output == nullptr) {
        ESP_LOGE(TAG, "Interpreter not initialized!");
        return result;
    }

    int elements = num_bytes / sizeof(float);
    int expected_elements = 1;
    for (int i = 0; i < input->dims->size; ++i) {
        expected_elements *= input->dims->data[i];
    }

    if (elements != expected_elements) {
        ESP_LOGE(TAG, "Size mismatch: Expected %d elements, got %d elements", expected_elements, elements);
        return result;
    }

    // Ép kiểu (Quantize) từ dữ liệu Float sang INT8
    if (input->type == kTfLiteInt8) {
        for (int i = 0; i < elements; i++) {
            // LƯU Ý: Dữ liệu từ imu_service đã được chuẩn hóa về khoảng [-1.0, 1.0]
            // Nên chúng ta KHÔNG cần kẹp và chia 8.0g hoặc 2000.0dps ở đây nữa.
            float val = rx_data[i];
            
            // Kẹp cứng vào giới hạn an toàn đề phòng ngoại lệ
            if (val > 1.0f) val = 1.0f;
            if (val < -1.0f) val = -1.0f;

            int32_t quantized_val = round(val / input->params.scale) + input->params.zero_point;
            // Kẹp (Clamp) giá trị vào giới hạn int8_t
            if (quantized_val > 127) quantized_val = 127;
            if (quantized_val < -128) quantized_val = -128;
            input->data.int8[i] = (int8_t)quantized_val;
        }
    } else if (input->type == kTfLiteFloat32) {
        memcpy(input->data.f, rx_data, num_bytes);
    } else {
        ESP_LOGE(TAG, "Unsupported input type: %d", input->type);
        return result;
    }

    int64_t start_time = esp_timer_get_time();
    TfLiteStatus invoke_status = interpreter->Invoke();
    int64_t end_time = esp_timer_get_time();
    
    if (invoke_status != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed!");
        return result;
    }

    result.inference_time_us = end_time - start_time;

    // Lấy số lượng class
    int num_classes = 1;
    if (output->dims->size > 1) {
        num_classes = output->dims->data[1];
    }

    // --- Giải Lượng tử hóa (Dequantize) Đầu Ra ---
    float probs[10]; // Giả sử model có tối đa 10 class
    if (num_classes > 10) num_classes = 10;
    
    for (int i = 0; i < num_classes; i++) {
        if (output->type == kTfLiteInt8) {
            probs[i] = (output->data.int8[i] - output->params.zero_point) * output->params.scale;
        } else {
            probs[i] = output->data.f[i];
        }
    }

    int predicted_class = 0;
    float max_prob = probs[0];
    for (int i = 1; i < num_classes; i++) {
        if (probs[i] > max_prob) {
            max_prob = probs[i];
            predicted_class = i;
        }
    }

    // --- Cập nhật: Logic 25% Threshold cho Fall ---
    const int FALL_CLASS_INDEX = 4; // 'Fall' là class thứ 5 theo CLASS_NAMES
    if (num_classes > FALL_CLASS_INDEX && probs[FALL_CLASS_INDEX] >= 0.25f) {
        predicted_class = FALL_CLASS_INDEX;
    }

    result.predicted_class = predicted_class;
    result.max_prob = max_prob;
    if (num_classes > FALL_CLASS_INDEX) {
        result.fall_prob = probs[FALL_CLASS_INDEX];
    }
    result.is_valid = true;

    return result;
}