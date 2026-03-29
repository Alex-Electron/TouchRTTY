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
        *bit_out = (p->integrate_count > 0) ? (p->integrate_acc / p->integrate_count) : disc;
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
    uint8_t       shift_reg;
    int           bit_count;
    bool          figs_mode;
    float         stop_acc;
    int           stop_samples;
    int           stop_needed;
    float         stop_bits;
    bool          unshift_on_space;
} baudot_framer_t;

inline void baudot_framer_init(baudot_framer_t *f, float stop_bits) {
    f->state     = FRAME_WAIT_START;
    f->shift_reg = 0;
    f->bit_count = 0;
    f->figs_mode = false;
    f->stop_acc  = 0.0f;
    f->stop_samples = 0;
    f->stop_needed = (stop_bits >= 1.5f) ? 2 : 1; // Number of sample bits to wait during stop
    f->stop_bits = stop_bits;
    f->unshift_on_space = true;
}

inline char baudot_framer_push(baudot_framer_t *f, float bit_value) {
    int bit = (bit_value > 0.0f) ? 1 : 0;

    switch (f->state) {
        case FRAME_WAIT_START:
            if (bit == 0) { // Start bit (Space)
                f->state = FRAME_RECV_DATA;
                f->shift_reg = 0;
                f->bit_count = 0;
                f->stop_acc = 0;
                f->stop_samples = 0;
            }
            break;

        case FRAME_RECV_DATA:
            f->shift_reg |= (bit << f->bit_count);
            if (++f->bit_count >= 5) {
                f->state = FRAME_RECV_STOP;
                f->stop_needed = (f->stop_bits >= 1.5f) ? 2 : 1;
                f->stop_acc = bit_value;
                f->stop_samples = 1;
                
                // Fast path for 1.0 stop bits - if the bit was 0, it means it already crashed into the next start bit!
                if (f->stop_needed == 1) {
                    f->state = FRAME_WAIT_START;
                    if (f->stop_acc < 0.0f) {
                        return '?'; // Framing error
                    }
                    uint8_t code = f->shift_reg & 0x1F;
                    if (code == 31) { f->figs_mode = false; return 0; }
                    if (code == 27) { f->figs_mode = true; return 0; }
                    char ch = f->figs_mode ? ita2_figs[code] : ita2_ltrs[code];
                    if (f->unshift_on_space && ch == ' ') f->figs_mode = false;
                    return ch ? ch : 0;
                }
            }
            break;

        case FRAME_RECV_STOP:
            f->stop_acc += bit_value;
            f->stop_samples++;

            if (f->stop_samples < f->stop_needed) break;

            f->state = FRAME_WAIT_START;

            if (f->stop_acc / f->stop_samples < -0.1f) {
                return '?'; // Framing error
            }

            uint8_t code = f->shift_reg & 0x1F;

            if (code == 31) {
                f->figs_mode = false;
                return 0;  
            }
            if (code == 27) {
                f->figs_mode = true;
                return 0;
            }

            char ch = f->figs_mode ? ita2_figs[code] : ita2_ltrs[code];

            if (f->unshift_on_space && ch == ' ')
                f->figs_mode = false;

            return ch ? ch : 0;
    }
    return 0;
}

#endif