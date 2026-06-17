#ifndef MODEL_DATA_H_
#define MODEL_DATA_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mảng byte chứa model TFLite đã lượng tử hóa INT8 (kiến trúc ResNet-1D).
 *
 * Model nhận đầu vào kích thước (200, 6) — 200 mẫu IMU, 6 kênh (gia tốc + con quay 3 trục)
 * và xuất ra 5 lớp phân loại (HAR / phát hiện ngã).
 *
 * @note File chứa dữ liệu (model_data.cc) được sinh tự động từ TFLite — KHÔNG sửa tay.
 */
extern const unsigned char g_model_data[];

/// Độ dài (số byte) của mảng g_model_data — dùng khi nạp model vào interpreter TFLite Micro.
extern const unsigned int g_model_data_len;

#ifdef __cplusplus
}
#endif

#endif // MODEL_DATA_H_
