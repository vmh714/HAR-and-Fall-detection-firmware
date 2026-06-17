#include "pedometer.h"
#include <math.h>

void pedometer_init(pedometer_t *ped, float fs_hz)
{
    float dt = 1.0f / fs_hz;

    /// High-pass 1 bậc, fc = 0.5Hz: loại thành phần trọng lực/DC và trôi chậm
    /// → tín hiệu dao động quanh 0.
    float rc_hp = 1.0f / (2.0f * (float)M_PI * 0.5f);
    ped->hp_alpha = rc_hp / (rc_hp + dt);

    /// Low-pass 1 bậc, fc = 3.5Hz: loại rung/nhiễu tần cao, giữ đúng dải nhịp bước
    /// (đi bộ ~1.5–2Hz, chạy ~2.5–3.3Hz).
    float rc_lp = 1.0f / (2.0f * (float)M_PI * 3.5f);
    ped->lp_alpha = dt / (rc_lp + dt);

    ped->hp_prev_in = 0.0f;
    ped->hp_prev_out = 0.0f;
    ped->lp_prev_out = 0.0f;
    ped->env_max = 0.0f;
    ped->env_min = 0.0f;
    ped->prev_bp = 0.0f;
    ped->samples_since_step = 0;

    /// Envelope bám biên độ gait trong ~2s gần đây.
    ped->decay = 1.0f / (fs_hz * 2.0f);
    /// Mặc định: trơ 250ms (≈4 bước/s) — caller có thể chỉnh theo class HAR.
    ped->refractory_samples = (uint32_t)(0.25f * fs_hz);
    /// Biên độ đỉnh-đỉnh tối thiểu (g) để loại rung nhỏ khi không thực sự bước.
    ped->min_p2p = 0.15f;
}

bool pedometer_process(pedometer_t *ped, float ax, float ay, float az)
{
    /// 1. Magnitude gia tốc — bất biến theo hướng đặt cảm biến.
    float mag = sqrtf(ax * ax + ay * ay + az * az);

    /// 2. High-pass: bỏ ~1g trọng lực + trôi → còn dao động quanh 0.
    float hp = ped->hp_alpha * (ped->hp_prev_out + mag - ped->hp_prev_in);
    ped->hp_prev_in = mag;
    ped->hp_prev_out = hp;

    /// 3. Low-pass: làm sạch nhiễu tần cao → còn đúng dải nhịp bước (band-pass hoàn chỉnh).
    float bp = ped->lp_prev_out + ped->lp_alpha * (hp - ped->lp_prev_out);
    ped->lp_prev_out = bp;

    /// 4. Cập nhật envelope max/min: bám tức thì khi vượt biên, suy giảm chậm khi không
    ///    → ngưỡng tự thích nghi theo biên độ bước của từng người/từng nhịp.
    if (bp > ped->env_max) ped->env_max = bp;
    else ped->env_max -= (ped->env_max - bp) * ped->decay;
    if (bp < ped->env_min) ped->env_min = bp;
    else ped->env_min += (bp - ped->env_min) * ped->decay;

    float threshold = (ped->env_max + ped->env_min) * 0.5f;
    float p2p = ped->env_max - ped->env_min;

    ped->samples_since_step++;

    /// 5. Một bước = cạnh XUỐNG cắt qua ngưỡng động (prev > thr ≥ now), với điều kiện:
    ///    biên độ đỉnh-đỉnh đủ lớn (loại nhiễu) và đã qua thời gian trơ (chống đếm đôi
    ///    do nhiều đỉnh con của cùng một sải bước).
    bool step = false;
    if (ped->prev_bp > threshold && bp <= threshold &&
        p2p > ped->min_p2p &&
        ped->samples_since_step >= ped->refractory_samples) {
        step = true;
        ped->samples_since_step = 0;
    }
    ped->prev_bp = bp;
    return step;
}
