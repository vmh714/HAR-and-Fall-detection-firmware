# Component: lib_tinyml (tflite_wrapper)

## Tổng Quan
Lớp bao bọc (Wrapper) tích hợp thư viện lõi TensorFlow Lite for Microcontrollers. Hoạt động độc lập, không dính dáng đến RTOS hay MQTT. Cấp phát Memory Arena trên PSRAM.

## Các Nhãn Phân Loại (`ai_posture_class_t`)
Enum định nghĩa trong `include/tflite_wrapper.h`, phải khớp với class list khi train model:

| Giá trị | Hằng số | Ý nghĩa |
|---|---|---|
| 0 | `AI_CLASS_WALK` | Đi bộ |
| 1 | `AI_CLASS_RUN` | Chạy |
| 2 | `AI_CLASS_IDLE` | Đứng yên / nghỉ |
| 3 | `AI_CLASS_TRANSITION` | Chuyển tư thế |
| 4 | `AI_CLASS_FALL` | Té ngã |
| 5 | `AI_CLASS_UNKNOWN` | Không xác định (giá trị khởi tạo / lỗi inference) |

## Các Struct Chính
```c
typedef struct {
  ai_posture_class_t predicted_class; // Nhãn class chiến thắng (enum, không phải int)
  float max_prob;            // Xác suất của predicted_class (0.0 - 1.0)
  float fall_prob;           // Xác suất của class Fall trong phân phối softmax
  int64_t inference_time_us; // Thời gian suy luận (microseconds)
  bool is_valid;             // true nếu inference thành công
} ai_inference_result_t;
```

> **Lưu ý về `fall_prob`**: Đây KHÔNG phải một xác suất độc lập riêng cho lớp Fall. Nó chỉ là một phần tử của vector softmax đầu ra — chính là `probs[AI_CLASS_FALL]` (tham chiếu `tflite_wrapper.cpp:242`). Tổng các phần tử softmax bằng 1.0.

## Logic Ngưỡng Ưu Tiên Fall (Fall Threshold)
Sau khi tính `argmax` để xác định class có xác suất cao nhất, wrapper áp thêm một bước ép kết quả vì lý do an toàn (tham chiếu `tflite_wrapper.cpp:234-235`):

- Nếu `probs[AI_CLASS_FALL] >= 0.6` (tức 60%) thì `predicted_class` bị **ép thành `AI_CLASS_FALL`** bất kể class nào đang thắng argmax.
- Mục đích: ưu tiên độ nhạy cho phát hiện té ngã, chấp nhận tăng nhẹ false-positive để giảm false-negative.

> Cảnh báo tài liệu: comment trong code ghi nhầm là "25% Threshold", nhưng giá trị thực thi là `0.6f` (60%). Tài liệu này lấy giá trị thực trong code làm chuẩn.

## Public API
- `int tflite_init(void)`: Khởi tạo target, cấp phát vùng Tensor Arena (100KB trên PSRAM qua `heap_caps_malloc(... MALLOC_CAP_SPIRAM)`), load model, đăng ký op, build `MicroInterpreter`, `AllocateTensors()`, và in ra dung lượng arena thực dùng (`arena_used_bytes()`). Trả về 0 nếu thành công, -1 nếu thất bại.
- `void tflite_run_inference(void)`: Chạy inference với dữ liệu giả lập (input điền toàn số 0) chỉ để đo và in thời gian suy luận. Không trả về kết quả phân loại — dùng cho benchmark/sanity-check.
- `int get_input_bytes(void)`: Trả về kích thước input theo **byte FLOAT32** (`tích các chiều của input->dims * sizeof(float)`), tức luôn tính theo float kể cả khi model là INT8 — để tương thích với data thật từ IMU/Python (tham chiếu `tflite_wrapper.cpp:136-142`).
- `ai_inference_result_t tflite_run_inference_with_data(float *data, size_t num_bytes)`: API lõi. Nhận mảng dữ liệu thô (đã chuẩn hóa về `[-1.0, 1.0]`), kiểm tra số phần tử khớp với input model, Lượng tử hóa (Quantize Float -> INT8) nếu model là INT8 (hoặc `memcpy` thẳng nếu FLOAT32), gọi `Invoke()`, Giải lượng tử hóa (Dequantize) phân phối xác suất, áp logic ngưỡng Fall, rồi trả về struct kết quả.

## Chi Tiết Mô Hình & Tensor
- **Input**: 3 trục — shape `[batch, window, 3]`, log ra `[%d, %d, %d]` (tham chiếu `tflite_wrapper.cpp:96-97`).
- **Output**: số class lấy động bằng `num_classes = output->dims->data[1]` (tham chiếu `tflite_wrapper.cpp:208-209`).
- **Tensor Arena**: `100 * 1024` byte (100KB) cấp phát trên PSRAM (tham chiếu `tflite_wrapper.cpp:22`). (Comment dòng 21 ghi "1MB" là sai sót comment trong code.)

## Các Toán Tử (Op Resolver)
Dùng `tflite::MicroMutableOpResolver<15>` với đúng 15 op được đăng ký cho model hiện tại (tham chiếu `tflite_wrapper.cpp:54-69`):

`Add`, `Concatenation`, `Conv2D`, `DepthwiseConv2D`, `ExpandDims`, `FullyConnected`, `Logistic`, `Mean`, `Mul`, `Pack`, `ReduceMax`, `Reshape`, `Shape`, `Softmax`, `StridedSlice`.

## Phụ Thuộc Build
- **IDF Component Manager** (`idf_component.yml`): `espressif/esp-tflite-micro: "^1.3.5"`.
- **CMakeLists.txt** `REQUIRES`: `lib_model` (chứa byte array model) + `esp_timer` (đo thời gian inference).

## Cập nhật cuối (Timestamp)
2026-06-17
