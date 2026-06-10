# Component: lib_model

## Tổng Quan
Kho lưu trữ mã nguồn C (.c / .h) chứa mảng nhị phân tĩnh của trọng số (weights) AI.

## Các API/Biến Chính
```c
extern const unsigned char g_model_data[];
extern const unsigned int g_model_data_len;
```

*Hoạt động*: Được trích xuất từ file `.tflite` qua tool `xxd`. Mảng `g_model_data` được hệ thống Linker nhúng trực tiếp vào phân vùng Flash (DROM), không tiêu tốn RAM, để `MicroInterpreter` đọc trực tiếp.
