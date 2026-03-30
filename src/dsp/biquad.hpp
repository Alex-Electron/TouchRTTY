#ifndef BIQUAD_HPP
#define BIQUAD_HPP

#include <math.h>

struct Biquad {
    float b0, b1, b2, a1, a2;
    float z1, z2;
};

inline void design_lpf(Biquad* f, float fc, float fs) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float alpha = sinf(w0) / (2.0f * 0.7071f);
    float cosw0 = cosf(w0);
    float A0 = 1.0f + alpha;
    f->b0 = ((1.0f - cosw0) * 0.5f) / A0;
    f->b1 = (1.0f - cosw0) / A0;
    f->b2 = ((1.0f - cosw0) * 0.5f) / A0;
    f->a1 = (-2.0f * cosw0) / A0;
    f->a2 = (1.0f - alpha) / A0;
    f->z1 = f->z2 = 0.0f;
}

inline float process_biquad(Biquad* f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

#endif