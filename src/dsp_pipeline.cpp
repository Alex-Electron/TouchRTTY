#include "dsp_pipeline.hpp"
#include "app_state.hpp"
#include "dsp/biquad.hpp"
#include "dsp/agc.hpp"
#include "dsp/ita2.hpp"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <algorithm>

float sin_table[1024];
float cos_table[1024];

const float fir_coeffs[FIR_TAPS] = {
    0.000167f, 0.000000f, 0.001438f, 0.000137f,
    -0.000722f, 0.001740f, -0.000000f, -0.002612f,
    0.001612f, -0.000447f, -0.006590f, 0.000000f,
    -0.001293f, -0.013294f, -0.004198f, -0.002266f,
    -0.022758f, -0.011889f, -0.002468f, -0.034228f,
    -0.023822f, -0.000000f, -0.046225f, -0.041377f,
    0.009206f, -0.056824f, -0.070578f, 0.038381f,
    -0.064124f, -0.160830f, 0.247760f, 0.600546f,
    0.247760f, -0.160830f, -0.064124f, 0.038381f,
    -0.070578f, -0.056824f, 0.009206f, -0.041377f,
    -0.046225f, -0.000000f, -0.023822f, -0.034228f,
    -0.002468f, -0.011889f, -0.022758f, -0.002266f,
    -0.004198f, -0.013294f, -0.001293f, 0.000000f,
    -0.006590f, -0.000447f, 0.001612f, -0.002612f,
    -0.000000f, 0.001740f, -0.000722f, 0.000137f,
    0.001438f, 0.000000f, 0.000167f,
};

void core0_dsp_loop() {
    multicore_lockout_victim_init();
    for(int i=0; i<1024; i++) {
        sin_table[i] = sinf(2.0f * (float)M_PI * (float)i / 1024.0f);
        cos_table[i] = cosf(2.0f * (float)M_PI * (float)i / 1024.0f);
    }

    // FIR buffer: power-of-2 size (64) for mask-based indexing instead of modulo
    static float ts[FFT_SIZE], tw[480], tw_m[480], tw_s[480], fb[64]={0.0f};
    int sc=0, wi=0, fi=0;
    static const int FIR_MASK = 63;

    // Hardware-paced ADC via FIFO (replaces blocking adc_read)
    // ADC runs free at exact 10kHz from hardware timer — zero jitter
    adc_init(); adc_gpio_init(ADC_PIN); adc_select_input(0);
    adc_fifo_setup(
        true,   // Write to FIFO
        false,  // Don't use DMA DREQ (yet — enable for Phase 8 DRM dual-channel)
        1,      // DREQ threshold (IRQ when 1+ samples ready)
        false,  // No ERR bit in FIFO
        false   // Don't shift to 8-bit
    );
    // Clock divider: 48MHz / (96 + 4704) = 10,000 Hz exactly
    adc_set_clkdiv(4704.0f);
    adc_run(true);  // Start free-running mode

    float dc=0.0f;

    float phase_m = 0.0f, phase_s = 0.0f;
    Biquad lp_mi, lp_mq, lp_si, lp_sq;
    float current_baud = -1.0f;
    float atc_mark_env = 0.01f, atc_space_env = 0.01f;
    float cached_atc_fast = 0.0f, cached_atc_slow = 0.0f; // Cached expf() results

    agc_t agc;
    agc_init(&agc, (float)SAMPLE_RATE);

    // Diagnostics
    float diag_adc_min = 4096.0f, diag_adc_max = 0.0f;
    float diag_m_pow = 0.0f, diag_s_pow = 0.0f;
    int diag_samples = 0;

    int baudot_state = 0;
    float symbol_phase = 0.0f;
    float integrate_acc = 0.0f;
    uint8_t current_char = 0;
    bool is_figs = false;
    bool last_d_sign = true;
    uint8_t err_hist[100] = {0};
    int err_idx = 0, err_sum = 0;
    float freq_error = 0.0f;

    // Auto-inversion detection
    uint32_t auto_inv_check_time = 0;  // when to check next
    uint32_t auto_inv_high_err_since = 0; // when high ERR started
    bool auto_inv_trying = false;      // currently testing inverted
    bool auto_inv_prev_inv = false;    // previous inversion state
    float auto_inv_pre_err = 0.0f;     // ERR before flip attempt

    // Auto stop-bit detection via gap measurement
    // Uses permissive setting (2.0) and measures time from stop-bit end to next start-bit
    int stop_detect_phase = 0;         // 0=idle, 1=measuring, 2=done
    uint32_t stop_detect_start = 0;    // when measurement started
    int stop_gap_hist[3] = {0, 0, 0};  // vote counts for 1.0/1.5/2.0 stop
    uint32_t stop_gap_state7_end_us = 0; // time when state 7 ended (0 = not armed)
    int stop_detect_saved_idx = 1;     // original stop idx before detection
    uint32_t auto_stop_high_err_since = 0; // AUTO-stop retry trigger tracker
    bool auto_recovery_pending_stop = false; // chain: baud-det → stop-det

    // Auto baud-rate detection via symbol duration histogram
    #define BAUD_HIST_BINS 350  // 0..35ms at 0.1ms (1 sample) resolution
    int baud_hist[BAUD_HIST_BINS] = {0};
    int baud_hist_sample_count = 0;      // total samples since detection start
    int baud_last_transition = -1;       // sample index of last D sign change
    bool baud_prev_d_sign = true;
    int baud_detect_phase = 0;           // 0=idle, 1=accumulating histogram, 2=verifying, 3=done
    uint32_t baud_detect_start = 0;
    int baud_detect_best_idx = 0;        // best baud index from histogram
    float baud_verify_err[4] = {100.0f, 100.0f, 100.0f, 100.0f};
    int baud_verify_phase = 0;           // which baud to verify (0..2), only used when verify needed

    // Core0 uses shared_active_shift for demodulation
    const float bauds[] = {45.45f, 50.0f, 75.0f, 100.0f};

    while(true) {
        // Wait for hardware-timed ADC sample (FIFO provides exact 10kHz timing)
        while (adc_fifo_is_empty()) {
            tight_loop_contents();
        }

        if (shared_clear_dsp) {
            shared_clear_dsp = false;
            shared_actual_freq = shared_target_freq;
            baudot_state = 0;
            symbol_phase = 0.0f;
            integrate_acc = 0.0f;
            freq_error = 0.0f;
            atc_mark_env = 0.01f;
            atc_space_env = 0.01f;
            shared_squelch_open = false;
            last_d_sign = true;
            // Reset error counter
            memset(err_hist, 0, sizeof(err_hist));
            err_idx = 0; err_sum = 0;
            shared_err_rate = 0.0f;
        }

        uint32_t st = time_us_32();
        uint16_t rv = adc_fifo_get_blocking(); // Hardware-timed, no software jitter

        if ((float)rv < diag_adc_min) diag_adc_min = (float)rv;
        if ((float)rv > diag_adc_max) diag_adc_max = (float)rv;

        if(rv<50 || rv>4045) shared_adc_clipping=true;
        float v = ((float)rv / 4095.0f) * 3.3f; shared_adc_v=v;
        float s = ((float)rv - 2048.0f) / 2048.0f;
        dc = dc * 0.99f + s * 0.01f; s -= dc;
        fb[fi] = s;
        // Symmetric FIR: coeff[i] == coeff[62-i], fold pairs → 32 MACs instead of 63
        // Buffer power-of-2 (64) lets us use bitmask instead of modulo
        float f_out = fir_coeffs[31] * fb[(fi - 31) & FIR_MASK];
        for (int i = 0; i < 31; i++) {
            f_out += fir_coeffs[i] * (fb[(fi - i) & FIR_MASK] + fb[(fi - 62 + i) & FIR_MASK]);
        }
        fi = (fi + 1) & FIR_MASK;

        float agc_out = agc_process(&agc, f_out);
        shared_agc_gain = agc.gain;

        if(wi<480) { tw[wi] = agc_out * 1.65f + 1.65f; }
        ts[sc++] = agc_out * 2.0f;

        f_out = agc_out;

        float baud = (shared_baud_idx < 4) ? bauds[shared_baud_idx] : shared_active_baud;
        static float current_k = -1.0f;
        float stop_bits_expected = (shared_stop_idx < 3) ? ((shared_stop_idx == 0) ? 1.0f : ((shared_stop_idx == 1) ? 1.5f : 2.0f)) : shared_active_stop;

        if (baud != current_baud || tuning_lpf_k != current_k) {
            current_baud = baud;
            current_k = tuning_lpf_k;
            float fc = baud * tuning_lpf_k;
            design_lpf(&lp_mi, fc, (float)SAMPLE_RATE); design_lpf(&lp_mq, fc, (float)SAMPLE_RATE);
            design_lpf(&lp_si, fc, (float)SAMPLE_RATE); design_lpf(&lp_sq, fc, (float)SAMPLE_RATE);
            // Cache expf() — was computed 10,000x/sec, now only on baud change
            cached_atc_fast = expf(-1.0f / (2.0f * ((float)SAMPLE_RATE / baud)));
            cached_atc_slow = expf(-1.0f / (16.0f * ((float)SAMPLE_RATE / baud)));
        }

        float shift = shared_active_shift;
        float fm = shared_actual_freq - shift / 2.0f;
        float fs = shared_actual_freq + shift / 2.0f;

        phase_m += fm * 0.0001f; if(phase_m >= 1.0f) phase_m -= 1.0f;
        phase_s += fs * 0.0001f; if(phase_s >= 1.0f) phase_s -= 1.0f;
        int idx_m = (int)(phase_m * 1024.0f) & 1023;
        int idx_s = (int)(phase_s * 1024.0f) & 1023;

        float mi = process_biquad(&lp_mi, f_out * cos_table[idx_m]);
        float mq = process_biquad(&lp_mq, f_out * sin_table[idx_m]);
        float si = process_biquad(&lp_si, f_out * cos_table[idx_s]);
        float sq = process_biquad(&lp_sq, f_out * sin_table[idx_s]);

        float mark_power = mi*mi + mq*mq;
        float space_power = si*si + sq*sq;

        if (wi < 480) { tw_m[wi] = mark_power; tw_s[wi] = space_power; wi++; }

        diag_m_pow += mark_power;
        diag_s_pow += space_power;
        diag_samples++;

        float new_m = sqrtf(mark_power + 1e-10f);
        float new_s = sqrtf(space_power + 1e-10f);
        // Use cached expf() values (computed only on baud change)
        float tc_m = (new_m > atc_mark_env) ? cached_atc_fast : cached_atc_slow;
        float tc_s = (new_s > atc_space_env) ? cached_atc_fast : cached_atc_slow;
        atc_mark_env  = atc_mark_env  * tc_m + new_m * (1.0f - tc_m);
        atc_space_env = atc_space_env * tc_s + new_s * (1.0f - tc_s);

        float m_norm = new_m / (atc_mark_env + 1e-6f);
        float s_norm = new_s / (atc_space_env + 1e-6f);

        float D = m_norm - s_norm;
        D = fmaxf(-1.5f, fminf(1.5f, D));
        if (shared_rtty_inv) D = -D;
        bool d_sign = (D > 0.0f);

        // Baud detection: accumulate symbol duration histogram
        if (baud_detect_phase == 1 && shared_squelch_open) {
            baud_hist_sample_count++;
            if (d_sign != baud_prev_d_sign) {
                if (baud_last_transition >= 0) {
                    int interval = baud_hist_sample_count - baud_last_transition;
                    if (interval > 0 && interval < BAUD_HIST_BINS)
                        baud_hist[interval]++;
                }
                baud_last_transition = baud_hist_sample_count;
            }
            baud_prev_d_sign = d_sign;
        }

        float phase_inc = baud / (float)SAMPLE_RATE;
        // freq_error declared above, before main loop
        float dpll_beta = tuning_dpll_alpha * tuning_dpll_alpha / 2.0f;

        if (!shared_squelch_open) {
            baudot_state = 0;
            last_d_sign = true;
            freq_error = 0.0f;
        } else {
            // Full PI loop DPLL
            if (d_sign != last_d_sign) {
                if (baudot_state > 0) {
                    float phase_error;
                    if (symbol_phase < 0.5f) phase_error = symbol_phase;
                    else phase_error = symbol_phase - 1.0f;

                    phase_error = fmaxf(-0.1f, fminf(0.1f, phase_error));
                    symbol_phase -= tuning_dpll_alpha * phase_error;
                    freq_error -= dpll_beta * phase_error;
                    float max_fe = 0.05f * phase_inc;
                    freq_error = fmaxf(-max_fe, fminf(max_fe, freq_error));
                } else if (!d_sign) {
                    // Transition to space = start bit
                    // Stop-bit gap measurement: time from state 7 end to this transition
                    if (stop_detect_phase == 1 && stop_gap_state7_end_us != 0) {
                        // Only count gap when decode is healthy (framer locked).
                        // If err_rate is high, state 7 end timing is meaningless.
                        if (shared_squelch_open && shared_err_rate < 10.0f) {
                            uint32_t gap_us = time_us_32() - stop_gap_state7_end_us;
                            float bit_period_us = 1000000.0f / baud;
                            float gap_fraction = (float)gap_us / bit_period_us;
                            // No LPF compensation: DPLL is locked to LPF-delayed transitions,
                            // so state-7-end wall clock and next-start-bit wall clock share the
                            // same LPF delay offset — it cancels out in the difference.
                            if (gap_fraction < 0.0f) gap_fraction = 0.0f;
                            shared_stop_gap_last = gap_fraction;
                            // Skip warmup: first 1.5s after STOP-DET start, DPLL phase
                            // hasn't stabilized and gap measurements are unreliable.
                            if (time_us_32() - stop_detect_start < 1500000) {
                                printf("[STOP-DET] gap=%lu us (%.3f T, bd=%.2f) WARMUP\n",
                                    (unsigned long)gap_us, (double)gap_fraction, (double)baud);
                            }
                            // Reject gaps > 1.25T as inter-frame/idle pauses.
                            else if (gap_fraction >= 1.25f) {
                                printf("[STOP-DET] gap=%lu us (%.3f T, bd=%.2f) SKIPPED (idle/break)\n",
                                    (unsigned long)gap_us, (double)gap_fraction, (double)baud);
                            } else {
                                int bin;
                                // Gap model (from state-7-end = end of 1st stop bit):
                                //   1.0 stop → gap ≈ 0T    → bin 0
                                //   1.5 stop → gap ≈ 0.5T  → bin 1
                                //   2.0 stop → gap ≈ 1.0T  → bin 2
                                // Boundaries: 0.25T and 0.85T (raised from 1.0 to avoid
                                // 2.0-stop gaps at ~0.94T landing in bin 1).
                                if (gap_fraction < 0.25f)      { bin = 0; stop_gap_hist[0]++; }
                                else if (gap_fraction < 0.85f) { bin = 1; stop_gap_hist[1]++; }
                                else                           { bin = 2; stop_gap_hist[2]++; }
                                shared_stop_gap_hist[0] = stop_gap_hist[0];
                                shared_stop_gap_hist[1] = stop_gap_hist[1];
                                shared_stop_gap_hist[2] = stop_gap_hist[2];
                                printf("[STOP-DET] gap=%lu us (%.3f T, bd=%.2f) bin=%d\n",
                                    (unsigned long)gap_us, (double)gap_fraction, (double)baud, bin);
                            }
                        }
                        stop_gap_state7_end_us = 0;
                    }
                    baudot_state = 1;
                    symbol_phase = 0.0f;
                    integrate_acc = 0.0f;
                    current_char = 0;
                }
            }
            last_d_sign = d_sign;

            if (baudot_state > 0) {
                symbol_phase += phase_inc + freq_error;
                integrate_acc += D;

                // Eye diagram: DPLL-locked accumulation (zero jitter)
                // symbol_phase 0..1 maps to x position within one bit period
                {
                    int spb = (int)((float)SAMPLE_RATE / baud);
                    if (spb > EYE_MAX_SPB) spb = EYE_MAX_SPB;
                    shared_eye_spb = spb;
                    int ex = (int)(symbol_phase * (float)(spb - 1));
                    if (ex >= 0 && ex < spb) {
                        int8_t dv = (int8_t)fmaxf(-127.0f, fminf(127.0f, D * 85.0f));
                        shared_eye_buf[shared_eye_idx][ex] = dv;
                    }
                }

                if (symbol_phase >= 1.0f) {
                    symbol_phase -= 1.0f;
                    // Advance eye trace on bit boundary
                    shared_eye_idx = (shared_eye_idx + 1) % EYE_TRACES;
                    shared_eye_ready = true;
                    bool bit = (integrate_acc > 0.0f);
                    integrate_acc = 0.0f;

                    if (baudot_state == 1) {
                        if (bit) baudot_state = 0; // False start
                        else baudot_state = 2;
                    } else if (baudot_state >= 2 && baudot_state <= 6) {
                        if (bit) current_char |= (1 << (baudot_state - 2));
                        baudot_state++;
                    } else if (baudot_state == 7) {
                        // Error rate tracking (100-char sliding window)

                        if (bit) { // Valid stop
                            char decoded = '\0';
                            if (current_char == 27) { is_figs = true; shared_figs_flag = true; }
                            else if (current_char == 31) { is_figs = false; shared_ltrs_flag = true; }
                            else {
                                decoded = is_figs ? ita2_figs[current_char] : ita2_ltrs[current_char];
                                if (decoded == ' ') is_figs = false;
                                if (decoded != '\0') {
                                    rtty_new_char = decoded;
                                    rtty_char_ready = true;
                                }
                            }
                            err_sum -= err_hist[err_idx]; err_hist[err_idx] = 0;
                            err_idx = (err_idx + 1) % 100;
                        } else {
                            shared_err_flag = true;
                            err_sum -= err_hist[err_idx]; err_hist[err_idx] = 1; err_sum++;
                            err_idx = (err_idx + 1) % 100;
                        }
                        shared_err_rate = (float)err_sum;

                        // Auto-inversion with comparative ERR logic
                        if (shared_inv_auto) {
                            uint32_t now = time_us_32();
                            // Clear uncertain flag when reception is clean
                            if (shared_err_rate < 5.0f && shared_squelch_open) shared_inv_uncertain = false;

                            if (shared_squelch_open && shared_err_rate > 12.0f && !auto_inv_trying) {
                                if (auto_inv_high_err_since == 0) auto_inv_high_err_since = now;
                                else if (now - auto_inv_high_err_since > 800000) {
                                    // High ERR for 0.8s — save current ERR, flip and measure
                                    auto_inv_pre_err = shared_err_rate;
                                    auto_inv_prev_inv = shared_rtty_inv;
                                    shared_rtty_inv = !shared_rtty_inv;
                                    auto_inv_trying = true;
                                    auto_inv_check_time = now + 800000;
                                    memset(err_hist, 0, sizeof(err_hist));
                                    err_idx = 0; err_sum = 0;
                                    shared_err_rate = 0.0f;
                                }
                            } else if (!auto_inv_trying && shared_err_rate <= 12.0f) {
                                auto_inv_high_err_since = 0;
                            }

                            if (auto_inv_trying && now > auto_inv_check_time) {
                                float new_err = shared_err_rate;
                                if (new_err < auto_inv_pre_err - 3.0f) {
                                    // ERR dropped significantly — keep new inversion
                                    shared_inv_uncertain = false;
                                } else if (new_err > auto_inv_pre_err + 3.0f) {
                                    // ERR got worse — revert
                                    shared_rtty_inv = auto_inv_prev_inv;
                                    shared_inv_uncertain = false;
                                } else {
                                    // ERR similar — can't determine, revert and mark uncertain
                                    shared_rtty_inv = auto_inv_prev_inv;
                                    shared_inv_uncertain = true;
                                }
                                auto_inv_trying = false;
                                auto_inv_high_err_since = 0;
                                memset(err_hist, 0, sizeof(err_hist));
                                err_idx = 0; err_sum = 0;
                                shared_err_rate = 0.0f;
                            }
                        }

                        // Arm stop-bit gap measurement ONLY on valid stop bit.
                        // Invalid stop = framer not locked → timestamp is meaningless,
                        // would pollute the histogram with garbage outliers.
                        if (stop_detect_phase == 1 && bit) {
                            stop_gap_state7_end_us = time_us_32();
                        }

                        if (stop_bits_expected <= 1.05f) {
                            // 1.0 stop bit: always enter start-bit state immediately
                            // (no gap between characters). Don't check !d_sign — the
                            // biquad LPF may not have crossed over yet. If idle (Mark),
                            // state 1 will detect false start and return to state 0.
                            baudot_state = 1;
                            symbol_phase = 0.0f;
                            integrate_acc = D;
                            current_char = 0;
                        } else {
                            baudot_state = 0;
                        }
                    }
                }
            }
        }

        if(sc==FFT_SIZE) {
            memcpy((void*)shared_fft_ts, ts, sizeof(ts));
            memcpy((void*)shared_adc_waveform, tw, sizeof(tw));
            memcpy((void*)shared_mag_m, tw_m, sizeof(tw_m));
            memcpy((void*)shared_mag_s, tw_s, sizeof(tw_s));
            new_data_ready=true;

            wi=0; memmove(ts, &ts[480], (FFT_SIZE-480)*sizeof(float)); sc=FFT_SIZE-480;

            static int diag_timer = 0;
            if (++diag_timer >= 10) {
                diag_timer = 0;
                shared_diag_adc_min = diag_adc_min;
                shared_diag_adc_max = diag_adc_max;
                shared_atc_m = atc_mark_env;
                shared_atc_s = atc_space_env;
                shared_dpll_phase = symbol_phase;
                shared_dpll_ferr = freq_error;
                shared_diag_ready = true;

                diag_adc_min = 4096.0f; diag_adc_max = 0.0f;
                diag_m_pow = 0.0f; diag_s_pow = 0.0f;
                diag_samples = 0;
            }

            // Auto stop-bit detection via direct gap measurement
            // Uses permissive 2.0 setting so framer waits for transition regardless of real stop bit
            uint32_t now_stop = time_us_32();

            // Auto-retry recovery chain: BAUD-DET → STOP-DET.
            // Wrong baud AND wrong stop-bit both cause framing errors; we can't tell
            // which is at fault, so re-measure both. Must sequence them: BAUD first
            // (so stop-det runs with known-good baud), then STOP.
            // Wait 3s so inv-toggle (which takes ~2s) gets a chance first.
            if (shared_stop_idx == 3 && shared_baud_auto
                && stop_detect_phase == 0 && !shared_stop_detect_req
                && baud_detect_phase == 0 && !shared_baud_detect_req
                && !auto_inv_trying && !auto_recovery_pending_stop
                && shared_squelch_open) {
                if (shared_err_rate > 15.0f) {
                    if (auto_stop_high_err_since == 0) auto_stop_high_err_since = now_stop;
                    else if (now_stop - auto_stop_high_err_since > 3000000) {
                        shared_baud_detect_req = true;
                        shared_baud_detect_state = 1;
                        auto_recovery_pending_stop = true;
                        auto_stop_high_err_since = 0;
                        printf("[AUTO-RECOVER] Triggered (ERR=%.1f%% > 15%% for 3s): BAUD-DET then STOP-DET\n",
                            (double)shared_err_rate);
                    }
                } else {
                    auto_stop_high_err_since = 0;
                }
            }

            // Chain: after BAUD-DET completes, fire STOP-DET.
            if (auto_recovery_pending_stop && baud_detect_phase == 0
                && !shared_baud_detect_req && shared_baud_detect_state == 3
                && stop_detect_phase == 0 && !shared_stop_detect_req) {
                shared_stop_detect_req = true;
                shared_stop_detect_state = 1;
                auto_recovery_pending_stop = false;
                printf("[AUTO-RECOVER] BAUD-DET done, starting STOP-DET\n");
            }

            // Chain from SEARCH: fire STOP-DET after BAUD-DET completes so STOP
            // classification uses the freshly-measured baud rate rather than a
            // stale default that mis-bins the gap fraction.
            if (shared_chain_stop_after_baud && baud_detect_phase == 0
                && !shared_baud_detect_req && shared_baud_detect_state == 3
                && stop_detect_phase == 0 && !shared_stop_detect_req) {
                shared_stop_detect_req = true;
                shared_stop_detect_state = 1;
                shared_chain_stop_after_baud = false;
                printf("[SEARCH-CHAIN] BAUD-DET done, starting STOP-DET\n");
            }

            if (shared_stop_detect_req && stop_detect_phase == 0) {
                stop_detect_saved_idx = shared_stop_idx;
                stop_detect_phase = 1;
                // Use permissive 2.0 setting (framer waits for start-bit transition)
                shared_stop_idx = 2;
                shared_active_stop = 2.0f;
                stop_gap_hist[0] = stop_gap_hist[1] = stop_gap_hist[2] = 0;
                shared_stop_gap_hist[0] = shared_stop_gap_hist[1] = shared_stop_gap_hist[2] = 0;
                stop_gap_state7_end_us = 0;
                memset(err_hist, 0, sizeof(err_hist)); err_idx = 0; err_sum = 0; shared_err_rate = 0.0f;
                auto_inv_trying = false; auto_inv_high_err_since = 0;
                stop_detect_start = now_stop;
                shared_stop_detect_req = false;
                shared_stop_detect_state = 1;
                printf("[STOP-DET] Waiting for stable signal then measuring (up to 8s)...\n");
            }
            if (stop_detect_phase == 1) {
                // Require minimum votes before finishing — need at least 20 good samples
                // Abort early only after long timeout (8s) to give weak signals a chance
                int total_votes = stop_gap_hist[0] + stop_gap_hist[1] + stop_gap_hist[2];
                bool enough_data = (total_votes >= 20);
                bool timeout = (now_stop - stop_detect_start > 8000000); // 8.0s hard limit
                if ((enough_data && (now_stop - stop_detect_start > 3000000)) || timeout) {
                    // Analyze histogram: pick bin with most votes
                    int best_idx = 1; // default 1.5
                    int best_votes = stop_gap_hist[1];
                    for (int i = 0; i < 3; i++) {
                        if (stop_gap_hist[i] > best_votes) {
                            best_votes = stop_gap_hist[i]; best_idx = i;
                        }
                    }
                    // If no votes at all, fallback to 1.5
                    if (best_votes == 0) best_idx = 1;
                    shared_stop_idx = 3; // keep AUTO mode marker
                    shared_active_stop = (best_idx==0)?1.0f:(best_idx==1)?1.5f:2.0f;
                    shared_stop_detect_state = 2;
                    stop_detect_phase = 0;
                    stop_gap_state7_end_us = 0;
                    memset(err_hist, 0, sizeof(err_hist)); err_idx = 0; err_sum = 0; shared_err_rate = 0.0f;
                    printf("[STOP-DET] Result: %.1f bits (votes: 1.0=%d 1.5=%d 2.0=%d)\n",
                        (double)shared_active_stop,
                        stop_gap_hist[0], stop_gap_hist[1], stop_gap_hist[2]);
                }
            }

            // Auto baud-rate detection state machine (histogram-based)
            uint32_t now_baud = time_us_32();
            if (shared_baud_detect_req && baud_detect_phase == 0) {
                // Start histogram accumulation
                memset(baud_hist, 0, sizeof(baud_hist));
                baud_hist_sample_count = 0;
                baud_last_transition = -1;
                baud_prev_d_sign = d_sign;
                baud_detect_phase = 1;
                baud_detect_start = now_baud;
                shared_baud_detect_req = false;
                shared_baud_detect_state = 1;
                printf("[BAUD-DET] Accumulating histogram (5s min, need 50+ transitions)...\n");
            }
            if (baud_detect_phase == 1) {
                // Count total non-zero histogram entries as transition count
                int total_transitions = 0;
                for (int b = 50; b < BAUD_HIST_BINS; b++) total_transitions += baud_hist[b];
                bool enough = (total_transitions >= 50);
                bool min_time = ((now_baud - baud_detect_start) > 5000000); // 5s minimum
                bool timeout = ((now_baud - baud_detect_start) > 10000000); // 10s hard limit
                if ((enough && min_time) || timeout) {
                // Analyze histogram: score each candidate baud rate
                // For each baud, check if histogram peaks align with multiples of bit_period
                const float cand_bauds[] = {45.45f, 50.0f, 75.0f, 100.0f};
                float baud_scores[4] = {0};
                int window = 10; // ±1.0ms tolerance (handles DPLL jitter ±5%)

                // Print histogram peaks for diagnostics
                printf("[BAUD-DET] Histogram (top intervals):");
                { struct { int bin; int count; } tops[5] = {}; int nt = 0;
                  for (int b = 50; b < BAUD_HIST_BINS; b++) {
                    if (baud_hist[b] > 0) {
                        int pos = nt < 5 ? nt++ : -1;
                        if (pos < 0 && baud_hist[b] > tops[4].count) pos = 4;
                        if (pos >= 0) {
                            tops[pos] = {b, baud_hist[b]};
                            for (int k = pos; k > 0 && tops[k].count > tops[k-1].count; k--)
                                std::swap(tops[k], tops[k-1]);
                        }
                    }
                  }
                  for (int i = 0; i < nt; i++)
                    printf(" %d(%.1fms,n=%d)", tops[i].bin, tops[i].bin*0.1f, tops[i].count);
                  printf("\n");
                }

                for (int ci = 0; ci < 4; ci++) {
                    float bit_period = (float)SAMPLE_RATE / cand_bauds[ci]; // samples per bit
                    // Score = sum of histogram values at k*bit_period ± window, k=1..5
                    for (int k = 1; k <= 5; k++) {
                        int center = (int)(bit_period * k + 0.5f);
                        for (int d = -window; d <= window; d++) {
                            int bin = center + d;
                            if (bin >= 0 && bin < BAUD_HIST_BINS) {
                                // Weight closer bins higher, earlier multiples higher
                                float dist_w = 1.0f - fabsf((float)d) / (float)(window + 1);
                                float mult_w = 1.0f / (float)k; // first multiple = full weight
                                baud_scores[ci] += baud_hist[bin] * dist_w * mult_w;
                            }
                        }
                    }
                    printf("[BAUD-DET] %.2f baud: period=%.1f samples, score=%.1f\n",
                        (double)cand_bauds[ci], (double)bit_period, (double)baud_scores[ci]);
                }

                // Pick best score
                baud_detect_best_idx = 0;
                for (int i = 1; i < 4; i++)
                    if (baud_scores[i] > baud_scores[baud_detect_best_idx])
                        baud_detect_best_idx = i;

                float best_score = baud_scores[baud_detect_best_idx];
                float second_best = 0;
                for (int i = 0; i < 4; i++)
                    if (i != baud_detect_best_idx && baud_scores[i] > second_best)
                        second_best = baud_scores[i];

                // If winner is clear (>1.5x second), apply directly
                // Otherwise verify with ERR test
                if (best_score > 5.0f && best_score > second_best * 1.5f) {
                    // Clear winner — apply immediately
                    shared_active_baud = cand_bauds[baud_detect_best_idx];
                    shared_baud_idx = 4; // keep AUTO
                    shared_baud_detect_state = 3; // done
                    baud_detect_phase = 0;
                    printf("[BAUD-DET] Result: %.2f baud (score=%.1f, clear winner)\n",
                        (double)shared_active_baud, (double)best_score);
                } else if (best_score > 1.0f) {
                    // Ambiguous — verify top 2 with ERR test
                    printf("[BAUD-DET] Ambiguous, verifying with ERR test...\n");
                    baud_detect_phase = 2;
                    baud_verify_phase = 0;
                    baud_verify_err[0] = baud_verify_err[1] = baud_verify_err[2] = 100.0f;
                    // Start testing baud 0 (45.45)
                    shared_active_baud = cand_bauds[0];
                    shared_baud_idx = 4; // keep AUTO
                    memset(err_hist, 0, sizeof(err_hist)); err_idx = 0; err_sum = 0; shared_err_rate = 0.0f;
                    auto_inv_trying = false; auto_inv_high_err_since = 0;
                    baud_detect_start = now_baud;
                    shared_baud_detect_state = 2;
                    printf("[BAUD-DET] Testing %.2f baud...\n", (double)cand_bauds[0]);
                } else {
                    // No signal / too weak — default to 45.45
                    shared_active_baud = 45.45f;
                    shared_baud_idx = 4;
                    shared_baud_detect_state = 3;
                    baud_detect_phase = 0;
                    printf("[BAUD-DET] No clear signal, defaulting to 45.45 baud\n");
                }
                } // close: if ((enough && min_time) || timeout)
            } // close: if (baud_detect_phase == 1)
            // ERR verification phase
            if (baud_detect_phase == 2) {
                const float cand_bauds_v[] = {45.45f, 50.0f, 75.0f, 100.0f};
                if (now_baud - baud_detect_start > 2000000) { // 2s per baud test
                    baud_verify_err[baud_verify_phase] = shared_err_rate;
                    printf("[BAUD-DET] %.2f baud: ERR=%.1f%%\n",
                        (double)cand_bauds_v[baud_verify_phase], (double)shared_err_rate);
                    baud_verify_phase++;
                    if (baud_verify_phase < 4) {
                        shared_active_baud = cand_bauds_v[baud_verify_phase];
                        memset(err_hist, 0, sizeof(err_hist)); err_idx = 0; err_sum = 0; shared_err_rate = 0.0f;
                        baud_detect_start = now_baud;
                        printf("[BAUD-DET] Testing %.2f baud...\n", (double)cand_bauds_v[baud_verify_phase]);
                    } else {
                        // Pick lowest ERR
                        int best = 0;
                        for (int i = 1; i < 4; i++)
                            if (baud_verify_err[i] < baud_verify_err[best]) best = i;
                        shared_active_baud = cand_bauds_v[best];
                        shared_baud_idx = 4; // keep AUTO
                        shared_baud_detect_state = 3;
                        baud_detect_phase = 0;
                        memset(err_hist, 0, sizeof(err_hist)); err_idx = 0; err_sum = 0; shared_err_rate = 0.0f;
                        printf("[BAUD-DET] Result: %.2f baud (ERR: 45=%.0f%% 50=%.0f%% 75=%.0f%% 100=%.0f%%)\n",
                            (double)shared_active_baud,
                            (double)baud_verify_err[0], (double)baud_verify_err[1], (double)baud_verify_err[2], (double)baud_verify_err[3]);
                    }
                }
            }
        }

        static uint32_t total_work = 0, total_time = 0;
        uint32_t work_end = time_us_32();
        total_work += (work_end - st);
        total_time += 100; // 100μs per sample at 10kHz
        if (total_time >= 500000) {
            shared_core0_load = (total_work * 100.0f) / (float)total_time;
            total_work = 0; total_time = 0;
        }
        // Hardware FIFO provides exact timing — no drain needed
        // (tight_loop_contents above ensures no overflow)
    }
}
