#ifndef LMS_NOTCH_HPP
#define LMS_NOTCH_HPP

#include <math.h>

// Nehorai-style constrained 2nd-order adaptive notch:
//   H(z) = (1 + a·z⁻¹ + z⁻²) / (1 + r·a·z⁻¹ + r²·z⁻²)
// Zeros on unit circle at ±θ where a = −2·cos(θ) (notch freq).
// Poles at radius r < 1 → notch bandwidth ≈ (1−r)·fs/π.
// LMS updates `a` to minimize E[y²], effectively tracking the dominant
// narrowband tone within its frequency window.
typedef struct {
    float a;         // current coefficient (−2·cos(w_notch))
    float r;         // pole radius (0.97..0.995)
    float mu;        // LMS step size
    float x1, x2;    // input delay line
    float y1, y2;    // output delay line (feedback)
    float a_lo, a_hi;// frequency-range constraint
} lms_notch_t;

static inline void lms_notch_init(lms_notch_t *n, float freq_hz, float fs,
                                  float r, float mu, float f_lo_hz, float f_hi_hz) {
    const float PI_F = 3.14159265358979f;
    float w = 2.0f * PI_F * freq_hz / fs;
    n->a = -2.0f * cosf(w);
    n->r = r;
    n->mu = mu;
    n->x1 = n->x2 = n->y1 = n->y2 = 0.0f;
    // cos is decreasing: higher freq → smaller cos → larger a (less negative).
    // a_lo corresponds to f_hi (upper band edge), a_hi to f_lo.
    n->a_lo = -2.0f * cosf(2.0f * PI_F * f_hi_hz / fs);
    n->a_hi = -2.0f * cosf(2.0f * PI_F * f_lo_hz / fs);
}

static inline float lms_notch_process(lms_notch_t *n, float x) {
    float r2 = n->r * n->r;
    float y = x + n->a * n->x1 + n->x2
              - n->r * n->a * n->y1 - r2 * n->y2;

    // LMS gradient approximation: dy/da ≈ x1 − r·y1
    float grad = n->x1 - n->r * n->y1;
    n->a -= n->mu * y * grad;

    // Clip to allowed frequency window (keeps notch out of the RTTY band
    // and prevents two cascaded notches from chasing the same QRM).
    if (n->a < n->a_lo) n->a = n->a_lo;
    if (n->a > n->a_hi) n->a = n->a_hi;

    n->x2 = n->x1; n->x1 = x;
    n->y2 = n->y1; n->y1 = y;
    return y;
}

#endif
