// TouchRTTY
// Copyright (C) 2026 Alexander Lavrinovich (Alex.Electron)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#ifndef DPLL_FRAMER_HPP
#define DPLL_FRAMER_HPP

#include <math.h>
#include <stdint.h>
#include "pico/stdlib.h"

// Full ITA2 maps
extern const char ita2_ltrs[32];
extern const char ita2_figs[32];

typedef struct {
    float   phase;          // накопленная фаза 0..1
    float   period;         // период символа в сэмплах = fs / baud
    float   alpha;          // gain петлевого фильтра
    float   beta;           // integral gain (alpha²/2)
    float   freq_error;     // накопленная частотная ошибка
    float   prev_disc;      // предыдущий дискриминатор
    int     sample_count;
    bool    bit_ready;
    float   integrate_acc;
    int     integrate_count;
} dpll_t;

inline void dpll_init(dpll_t *p, float baud_rate, float fs) {
    p->period      = fs / baud_rate;
    p->phase       = 0.5f;   // начать с середины первого бита
    p->alpha       = 0.035f; // ширина петли захвата
    p->beta        = p->alpha * p->alpha / 2.0f;
    p->freq_error  = 0.0f;
    p->prev_disc   = 0.0f;
    p->sample_count = 0;
    p->bit_ready   = false;
    p->integrate_acc = 0.0f;
    p->integrate_count = 0;
}

inline bool dpll_process(dpll_t *p, float disc, float *bit_out) {
    p->phase += 1.0f / p->period + p->freq_error;
    p->bit_ready = false;
    
    p->integrate_acc += disc;
    p->integrate_count++;

    // Детект перехода бита (zero-crossing):
    bool transition = (disc * p->prev_disc < 0.0f);
    p->prev_disc = disc;

    if (transition) {
        float phase_error;
        if (p->phase < 0.5f) {
            phase_error =  p->phase;         // слишком рано
        } else {
            phase_error = p->phase - 1.0f;   // слишком поздно
        }

        // Ограничить коррекцию:
        phase_error = fmaxf(-0.1f, fminf(0.1f, phase_error));

        // Петлевой фильтр:
        p->phase       -= p->alpha * phase_error;
        p->freq_error  -= p->beta  * phase_error;

        // Ограничить частотную ошибку (±5%):
        p->freq_error = fmaxf(-0.05f / p->period,
                        fminf( 0.05f / p->period, p->freq_error));
    }

    // Решение в конце символа:
    if (p->phase >= 1.0f) {
        p->phase -= 1.0f;
        *bit_out = (p->integrate_count > 0) ? (p->integrate_acc / (float)p->integrate_count) : disc;
        p->integrate_acc = 0.0f;
        p->integrate_count = 0;
        p->bit_ready = true;
        return true;
    }
    return false;
}

typedef enum {
    FRAME_WAIT_START = 0,
    FRAME_RECV_DATA,
    FRAME_RECV_STOP
} frame_state_t;

typedef struct {
    frame_state_t state;
    int           bit_count;
    bool          figs_mode;
    // Soft storage: raw bit_value LLRs from dpll per bit.
    float         data_soft[5];   // 5 data bits (LSB first)
    float         start_soft;     // start bit (expected: negative / space)
    float         stop_acc;       // accumulated stop-bit soft (expected: positive / mark)
    int           stop_samples;
    int           stop_needed;
    float         stop_bits;
    bool          unshift_on_space;
    // Running EMA of |bit_value| — proxy for signal level. Adaptive thresholds
    // normalize to this so rejection survives AGC drift and Mark/Space imbalance.
    float         sig_level;
} baudot_framer_t;

inline void baudot_framer_init(baudot_framer_t *f, float stop_bits) {
    f->state     = FRAME_WAIT_START;
    f->bit_count = 0;
    f->figs_mode = false;
    for (int i = 0; i < 5; i++) f->data_soft[i] = 0.0f;
    f->start_soft = 0.0f;
    f->stop_acc  = 0.0f;
    f->stop_samples = 0;
    f->stop_needed = (stop_bits >= 1.5f) ? 2 : 1;
    f->stop_bits = stop_bits;
    f->unshift_on_space = true;
    f->sig_level = 0.1f;  // bootstrap
}

// Emit final char from collected soft values. Called when stop-window full.
// Applies soft-LLR: hard-slice happens only at frame boundary, and the
// decision is rejected if stop magnitude is tiny relative to signal level
// (adaptive threshold beats the old fixed −0.1 check at varying SNR/AGC).
static inline char _baudot_frame_emit(baudot_framer_t *f) {
    f->state = FRAME_WAIT_START;

    float stop_mean = f->stop_acc / (float)f->stop_samples;

    // Soft-Viterbi frame validation (B242 + B243):
    //  (1) stop must look like MARK (positive), magnitude ≥ 25% of signal level.
    //  (2) start must look like SPACE (negative), magnitude ≥ 15% of signal level.
    //  (3) B243: weakest data-bit confidence ≥ 20% of signal level.
    //  (4) B243: frame-average confidence ≥ 30% of signal level.
    const float STOP_MIN_FRAC  = 0.25f;
    const float START_MIN_FRAC = 0.15f;
    const float DATA_MIN_FRAC  = 0.10f;
    const float FRAME_AVG_FRAC = 0.15f;
    if (stop_mean < STOP_MIN_FRAC * f->sig_level)            return '?';
    if (-f->start_soft < START_MIN_FRAC * f->sig_level)      return '?';

    // Soft-bit confidence check: reject if any data bit is near zero.
    float data_min = fabsf(f->data_soft[0]);
    float frame_sum = fabsf(f->start_soft) + stop_mean;
    for (int i = 0; i < 5; i++) {
        float a = fabsf(f->data_soft[i]);
        if (a < data_min) data_min = a;
        frame_sum += a;
    }
    if (data_min < DATA_MIN_FRAC * f->sig_level)             return '?';
    if ((frame_sum / 7.0f) < FRAME_AVG_FRAC * f->sig_level)  return '?';

    // Hard-slice 5 data bits from soft values (LSB first).
    uint8_t code = 0;
    for (int i = 0; i < 5; i++) {
        if (f->data_soft[i] > 0.0f) code |= (1 << i);
    }

    if (code == 31) { f->figs_mode = false; return 0; }
    if (code == 27) { f->figs_mode = true;  return 0; }

    char ch = f->figs_mode ? ita2_figs[code] : ita2_ltrs[code];
    if (f->unshift_on_space && ch == ' ') f->figs_mode = false;
    return ch ? ch : 0;
}

inline char baudot_framer_push(baudot_framer_t *f, float bit_value) {
    // Track running signal level (mean absolute) for adaptive thresholds.
    float abs_bv = fabsf(bit_value);
    f->sig_level = 0.98f * f->sig_level + 0.02f * abs_bv;

    switch (f->state) {
        case FRAME_WAIT_START:
            if (bit_value < 0.0f) { // start bit = Space
                f->state = FRAME_RECV_DATA;
                f->start_soft = bit_value;
                f->bit_count = 0;
                f->stop_acc = 0.0f;
                f->stop_samples = 0;
            }
            break;

        case FRAME_RECV_DATA:
            f->data_soft[f->bit_count] = bit_value;
            if (++f->bit_count >= 5) {
                f->state = FRAME_RECV_STOP;
                f->stop_needed = (f->stop_bits >= 1.5f) ? 2 : 1;
                f->stop_acc = bit_value;       // first post-data sample IS a stop sample
                f->stop_samples = 1;
                if (f->stop_needed == 1) return _baudot_frame_emit(f);
            }
            break;

        case FRAME_RECV_STOP:
            f->stop_acc += bit_value;
            f->stop_samples++;
            if (f->stop_samples < f->stop_needed) break;
            return _baudot_frame_emit(f);
    }
    return 0;
}

#endif