# Component: lib_tinyml (tflite_wrapper)

> **Cập nhật cuối (Timestamp):** 2026-06-30
> **Trạng thái:** Hoạt động ổn định

---

## 1. PUBLIC API & CẤU TRÚC DỮ LIỆU

### 1.1 Khởi tạo và Suy luận (Inference)
- **`int tflite_init(void)`**: Khởi tạo target, cấp phát vùng Tensor Arena trên PSRAM, load model, đăng ký op, build `MicroInterpreter` và `AllocateTensors()`. Trả về 0 nếu thành công, -1 nếu thất bại.
- **`ai_inference_result_t tflite_run_inference_with_data(float *data, size_t num_bytes)`**: API lõi. Nhận mảng dữ liệu thô (đã chuẩn hóa), Lượng tử hóa (Quantize Float -> INT8) nếu model là INT8, gọi `Invoke()`, Giải lượng tử hóa (Dequantize) phân phối xác suất, áp logic ngưỡng Fall, rồi trả về struct kết quả.
- **`int get_input_bytes(void)`**: Trả về kích thước input theo **byte FLOAT32** để tương thích với dữ liệu từ IMU (`tích các chiều của input->dims * sizeof(float)`).

### 1.2 Các Struct và Enum Chính
```c
typedef enum {
    AI_CLASS_WALK = 0,
    AI_CLASS_RUN = 1,
    AI_CLASS_IDLE = 2,
    AI_CLASS_TRANSITION = 3,
    AI_CLASS_FALL = 4,
    AI_CLASS_UNKNOWN = 5
} ai_posture_class_t;

typedef struct {
  ai_posture_class_t predicted_class; // Nhãn class chiến thắng
  float max_prob;            // Xác suất của predicted_class (0.0 - 1.0)
  float fall_prob;           // Xác suất của class Fall trong phân phối softmax
  int64_t inference_time_us; // Thời gian suy luận (microseconds)
  bool is_valid;             // true nếu inference thành công
} ai_inference_result_t;
```

---

## 2. Mục đích (Purpose / Why)
Lớp bao bọc (Wrapper) tích hợp thư viện lõi **TensorFlow Lite for Microcontrollers**. Nó giúp hệ thống giao tiếp với bộ não AI mà không cần quan tâm đến các chi tiết phức tạp của việc cấp phát memory, đăng ký operator, lượng tử hóa và giải mã tensor đầu ra.

## 3. Cơ chế cốt lõi (How it works)

### 3.1 Logic Ngưỡng Ưu Tiên Fall (Fall Threshold)
Sau khi tính `argmax` để xác định class có xác suất cao nhất, wrapper áp thêm một bước ép kết quả vì lý do an toàn:
- Nếu xác suất của lớp Fall (`probs[AI_CLASS_FALL]`) **`>= 0.6` (60%)** thì `predicted_class` bị **ép thành `AI_CLASS_FALL`** bất kể class nào đang thắng argmax.
- Mục đích: Ưu tiên độ nhạy (Recall) cho phát hiện té ngã, chấp nhận tăng nhẹ false-positive để giảm false-negative, bảo vệ tính mạng người dùng.

### 3.2 Tensor Arena & Toán Tử (Op Resolver)
- **Tensor Arena**: `100 * 1024` byte (100KB) cấp phát trên PSRAM (thông qua `heap_caps_malloc(... MALLOC_CAP_SPIRAM)`).
- **Op Resolver**: Dùng `tflite::MicroMutableOpResolver<15>` với đúng 15 op được đăng ký cho model để tiết kiệm bộ nhớ: `Add`, `Concatenation`, `Conv2D`, `DepthwiseConv2D`, `ExpandDims`, `FullyConnected`, `Logistic`, `Mean`, `Mul`, `Pack`, `ReduceMax`, `Reshape`, `Shape`, `Softmax`, `StridedSlice`.

### 3.3 Chi Tiết Mô Hình
- **Input**: 3 trục — shape `[batch, window, 3]`.
- **Output**: số class lấy động bằng `num_classes = output->dims->data[1]`.

## 4. Đa nhiệm & Tài nguyên (Concurrency & Resources)
- **Thuần thuật toán**: Hoạt động độc lập, hoàn toàn đồng bộ (synchronous), không dính dáng đến RTOS hay MQTT. Thời gian block CPU phụ thuộc vào độ phức tạp của mô hình (được đo bởi `inference_time_us`).
- **RAM**: Tensor Arena được cấp trên PSRAM để không làm cạn kiệt Internal SRAM, giúp các kết nối WiFi/4G không bị lỗi thiếu RAM.

## 5. Luồng giao tiếp (Data Flow)
- Thư viện không tự gọi, mà được `svc_ai` triệu gọi khi có đủ 200 mẫu IMU (Sliding Window).
- **Phụ Thuộc Build**: 
  - Yêu cầu component `espressif/esp-tflite-micro: "^1.3.5"`.
  - Phụ thuộc `lib_model` (chứa byte array model) và `esp_timer` (để đo thời gian).
