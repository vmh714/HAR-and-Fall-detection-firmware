# Component: lib_pedometer

## Tổng Quan
Thư viện **thuần thuật toán** (platform-independent, giống `lib_kalman`) để đếm bước chân từ dữ liệu gia tốc. Không phụ thuộc phần cứng, FreeRTOS hay HAR — việc gate (chỉ đếm khi đi/chạy) và gán nhãn walk/run do tầng gọi (`svc_imu`) quyết định.

## Thuật toán
1. **Magnitude** `|a| = √(ax²+ay²+az²)` — bất biến theo hướng đặt cảm biến.
2. **Band-pass 0.5–3.5Hz** (high-pass 1 bậc loại trọng lực/trôi + low-pass 1 bậc loại rung tần cao) → tách đúng dải nhịp gait (đi bộ ~1.5–2Hz, chạy ~2.5–3.3Hz). Hệ số lọc tính từ `fs` trong `pedometer_init`.
3. **Ngưỡng động** = trung điểm envelope max/min (bám nhanh khi vượt, suy giảm chậm ~2s) → tự thích nghi biên độ từng người/nhịp.
4. **Peak-detect**: một bước = cạnh xuống cắt qua ngưỡng, với biên độ đỉnh-đỉnh `> min_p2p` và đã qua **thời gian trơ** (`refractory_samples`) chống đếm đôi do nhiều đỉnh con của một sải.

## Public API
- **`void pedometer_init(pedometer_t *ped, float fs_hz)`** — khởi tạo, tính hệ số lọc theo tần số lấy mẫu. Mặc định trơ 250ms (~4 bước/s), `min_p2p = 0.15g`.
- **`bool pedometer_process(pedometer_t *ped, float ax, float ay, float az)`** — xử lý một mẫu accel (đơn vị **g**), trả `true` nếu mẫu này tạo thành một bước.

## Lưu ý tích hợp
- Nên cấp **accel THÔ** (chưa qua bộ lọc làm mượt như Kalman) để đỉnh bước rõ.
- Nên gọi `pedometer_process` **mỗi mẫu** (kể cả khi đứng yên) để bộ lọc liên tục, tránh transient.
- Caller có thể chỉnh `ped->refractory_samples` theo nhịp (vd Walk ~300ms, Run ~200ms) và `ped->min_p2p` sau khi init.

> **Cập nhật cuối (Timestamp):** 2026-06-17
