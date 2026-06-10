# Component: lib_tinyml (tflite_wrapper)

## Tổng Quan
Lớp bao bọc (Wrapper) tích hợp thư viện lõi TensorFlow Lite for Microcontrollers. Hoạt động độc lập, không dính dáng đến RTOS hay MQTT. Cấp phát Memory Arena trên PSRAM.

## Các Struct Chính
```c
typedef struct {
  int predicted_class;       // 0: Walk, 1: Run, 2: Idle, 3: Stairs, 4: Fall
  float max_prob;            // Xác suất của predicted_class (0.0 - 1.0)
  float fall_prob;           // Xác suất độc lập của class Fall
  int64_t inference_time_us; // Thời gian suy luận
  bool is_valid;             
} ai_inference_result_t;
```

## Public API
- `int tflite_init(void)`: Khởi tạo mô hình, cấp phát vùng Tensor Arena (100KB trên PSRAM), cấu hình `MicroMutableOpResolver`, báo cáo bộ nhớ dư thừa.
- `int get_input_bytes(void)`: Tiện ích tính toán kích thước input mà model yêu cầu.
- `ai_inference_result_t tflite_run_inference_with_data(float *data, size_t num_bytes)`: API lõi. Nhận mảng dữ liệu thô, thực hiện Lượng tử hóa (Quantize Float -> INT8), gọi `Invoke()`, và Giải lượng tử hóa (Dequantize) kết quả phân phối xác suất. Trả về Struct chứa thông tin class chiến thắng.
