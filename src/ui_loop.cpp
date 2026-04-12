#include "ui_loop.hpp"
#include "app_state.hpp"
#include "settings_flash.hpp"
#include "serial_commands.hpp"
#include "dsp_pipeline.hpp"
#include "LGFX_Config.hpp"
#include "display/ili9488_driver.h"
#include "dsp/fft.hpp"
#include <algorithm>
#include "ui/UIManager.hpp"
#include "version.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

void core1_main() {
    LGFX_RP2350 tft; tft.init(); tft.setRotation(1);

    load_or_calibrate(tft, shared_force_cal);

    ili9488_init();    ili9488_init_waterfall_lut();    ili9488_fill_screen(0x0000);
    UIManager ui(&tft); ui.init();
    
    bool auto_scale = true;
    bool menu_mode = false;
    bool diag_screen_active = false;
    bool tuning_lab_active = false;
    int display_mode = 0;
    bool reset_confirm_mode = false;
    bool shift_popup_active = false;
    bool stop_popup_active = false;
    bool baud_popup_active = false;
    uint32_t saved_text_timer = 0;
    
    AppSettings loaded_set;
    memcpy(&loaded_set, flash_settings_contents, sizeof(AppSettings));
    if (loaded_set.magic == 0xDEADBEEF) {
        shared_baud_idx = (loaded_set.baud_idx >= 0 && loaded_set.baud_idx <= 4) ? loaded_set.baud_idx : 0;
        shared_baud_auto = (shared_baud_idx == 4);
        shared_shift_idx = (loaded_set.shift_idx >= 0 && loaded_set.shift_idx <= NUM_SHIFTS) ? loaded_set.shift_idx : 1; // default 170Hz
        shared_stop_idx = loaded_set.stop_idx;
        shared_stop_auto = (shared_stop_idx == 3) || loaded_set.stop_auto;
        shared_inv_auto = loaded_set.inv_auto;
        shared_rtty_inv = loaded_set.rtty_inv;
        display_mode = loaded_set.display_mode;
        shared_exp_scale = loaded_set.exp_scale;
        auto_scale = loaded_set.auto_scale;
        tuning_lpf_k = loaded_set.filter_k;
        tuning_sq_snr = loaded_set.sq_snr;
        shared_target_freq = loaded_set.target_freq;
        shared_actual_freq = shared_target_freq;
        shared_serial_diag = loaded_set.serial_diag; 
        shared_line_width = (loaded_set.line_width >= 30 && loaded_set.line_width <= 80) ? loaded_set.line_width : 60;
        shared_afc_on = loaded_set.afc_on;
        shared_font_mode = loaded_set.font_mode;
        if (loaded_set.dpll_alpha >= 0.005f && loaded_set.dpll_alpha <= 0.200f)
            tuning_dpll_alpha = loaded_set.dpll_alpha;
    }
    
    const float bauds[] = {45.45f, 50.0f, 75.0f, 100.0f};
    const float stop_bits[] = {1.0f, 1.5f, 2.0f};
    // Active shift: from g_shifts[idx] or auto-detected
    auto get_shift = []() -> float {
        if (shared_shift_idx < NUM_SHIFTS) { shared_active_shift = g_shifts[shared_shift_idx]; return shared_active_shift; }
        return shared_active_shift;
    };
    auto get_stop = [&]() -> float {
        if (shared_stop_idx < 3) { shared_active_stop = stop_bits[shared_stop_idx]; return shared_active_stop; }
        return shared_active_stop; // AUTO — use last detected
    };
    auto get_baud = [&]() -> float {
        if (shared_baud_idx < 4) { shared_active_baud = bauds[shared_baud_idx]; return shared_active_baud; }
        return shared_active_baud; // AUTO — use last detected
    };

    ui.drawBottomBar(shared_baud_idx, shared_shift_idx, get_stop(), shared_afc_on, menu_mode, shared_search_state, shared_stop_auto, shared_baud_auto);

    LGFX_Sprite spectrum(&tft); spectrum.setColorDepth(16); spectrum.createSprite(480, UI_DSP_ZONE_H);
    LGFX_Sprite marker_spr(&tft); marker_spr.setColorDepth(16); marker_spr.createSprite(480, UI_MARKER_H);

    // Waterfall circular history buffer: uint8 magnitudes (0-255), LUT converts to color
    static uint8_t wf_history[UI_DSP_ZONE_H * 480] = {0};
    int wf_offset = UI_DSP_ZONE_H - 1;
    
    const int bin_start = 5, bin_end = 358;
    const float bin_per_pixel = (float)(bin_end - bin_start) / 480.0f;
    uint32_t last_touch = time_us_32(), last_ui_update = time_us_32(), frame_count = 0;
    float local_mag[FFT_SIZE / 2], local_wave[480], local_mag_m[480], local_mag_s[480], local_ts[FFT_SIZE];
    int16_t tune_x = 240;
    float ui_noise_floor = -60.0f, ui_gain = 0.0f;
    static float smooth_mag[FFT_SIZE / 2] = {0};

    static SimpleFFT fft; static float real[FFT_SIZE], imag[FFT_SIZE], mag[FFT_SIZE/2];

    uint32_t c1_total_work = 0;
    uint32_t search_result_time = 0;
    uint32_t c1_last_measure = time_us_32();

    while (true) {
        uint32_t loop_start = time_us_32();
        handle_serial_commands();

        // Handle save request from serial command
        if (shared_save_request) {
            shared_save_request = false;
            AppSettings s;
            s.magic = 0xDEADBEEF;
            s.baud_idx = shared_baud_idx; s.shift_idx = shared_shift_idx;
            s.stop_idx = shared_stop_idx; s.rtty_inv = shared_rtty_inv;
            s.display_mode = display_mode; s.exp_scale = shared_exp_scale;
            s.auto_scale = auto_scale; s.filter_k = tuning_lpf_k;
            s.sq_snr = tuning_sq_snr; s.target_freq = shared_target_freq;
            s.serial_diag = shared_serial_diag; s.line_width = shared_line_width;
            s.afc_on = shared_afc_on; s.font_mode = shared_font_mode; s.dpll_alpha = tuning_dpll_alpha;
            s.inv_auto = shared_inv_auto; s.stop_auto = shared_stop_auto;
            uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
            memcpy(page_buf, &s, sizeof(AppSettings));
            multicore_lockout_start_blocking();
            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
            flash_range_program(SETTINGS_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
            restore_interrupts(ints);
            multicore_lockout_end_blocking();
            settings_need_save = false;
            printf(">> SAVED OK\n");
        }

        if (rtty_char_ready) {
            char c = rtty_new_char;
            ui.addRTTYChar(c, !diag_screen_active && !menu_mode && !tuning_lab_active && !reset_confirm_mode && !shift_popup_active && !stop_popup_active && !baud_popup_active);
            rtty_char_ready = false;
            // Only output to Serial if diagnostics are OFF
            if (!shared_serial_diag) printf("%c", c);
        }
        
        if (shared_err_flag) { 
            shared_err_flag = false; 
            if (!shared_serial_diag) printf("[ERR]"); 
        }
        if (shared_figs_flag) { 
            shared_figs_flag = false; 
            if (!shared_serial_diag) printf("[FIGS]"); 
        }
        if (shared_ltrs_flag) { 
            shared_ltrs_flag = false; 
            if (!shared_serial_diag) printf("[LTRS]"); 
        }

        if (shared_diag_ready) {
            shared_diag_ready = false;
            if (shared_serial_diag) {
                // Compact diagnostic stream — one line per update, easy to parse
                float agc_db = 20.0f * log10f(shared_agc_gain + 1e-10f);
                const int bauds_d[] = {45, 50, 75, 100};
                printf("[D] SNR=%.1f SIG=%.1f ERR=%.0f%% SQ=%s AGC=%+.0fdB PH=%.2f FE=%.5f M=%.3f S=%.3f A=%.4f K=%.2f SQT=%.1f F=%.0f B=%d C0=%d%% C1=%d%%\n",
                    (double)shared_snr_db, (double)shared_signal_db,
                    (double)shared_err_rate,
                    shared_squelch_open ? "OPEN" : "SHUT",
                    (double)agc_db, (double)shared_dpll_phase,
                    (double)shared_dpll_ferr,
                    (double)shared_atc_m, (double)shared_atc_s,
                    (double)tuning_dpll_alpha, (double)tuning_lpf_k,
                    (double)tuning_sq_snr, (double)shared_actual_freq,
                    bauds_d[shared_baud_idx],
                    (int)shared_core0_load, (int)shared_core1_load);
            }
        }

        float bin_idx = shared_actual_freq / (SAMPLE_RATE / (float)FFT_SIZE);
        tune_x = (int)((bin_idx - bin_start) / bin_per_pixel);
        tune_x = std::clamp((int)tune_x, 10, 470);

        if (loop_start - last_ui_update > 200000) {
            uint32_t fps = frame_count * 5; frame_count = 0; last_ui_update = loop_start;
            float m_freq = shared_actual_freq - get_shift()/2.0f;
            float s_freq = shared_actual_freq + get_shift()/2.0f;
            static uint32_t clip_latch_until = 0;
            if (shared_adc_clipping) { clip_latch_until = loop_start + 1500000; shared_adc_clipping = false; }
            bool is_clipping = (loop_start < clip_latch_until);
            
            if (tuning_lab_active) {
                ui.drawTuningControls(tuning_dpll_alpha, tuning_lpf_k, tuning_sq_snr, shared_err_rate);
            } else if (diag_screen_active) {
                ui.drawDiagScreen(shared_adc_v, shared_font_mode);
            }
            
            ui.updateTopBar(shared_adc_v, fps, shared_signal_db, shared_snr_db, m_freq, s_freq, is_clipping, shared_core0_load, shared_core1_load, shared_squelch_open, shared_agc_gain, shared_agc_enabled, shared_err_rate, shared_rtty_inv, shared_shift_idx, shared_active_shift, shared_inv_uncertain, shared_stop_auto, shared_active_stop, shared_stop_detect_state, shared_baud_auto, shared_active_baud, shared_baud_detect_state);
            // Auto-clear search result after 2 seconds + redraw bottom bar on state change
            static int prev_search_state = 0;
            if (shared_search_state >= 2 && search_result_time > 0 && (loop_start - search_result_time > 2000000)) {
                shared_search_state = 0;
            }
            if (shared_search_state != prev_search_state) {
                prev_search_state = shared_search_state;
                if (!tuning_lab_active && !diag_screen_active)
                    ui.drawBottomBar(shared_baud_idx, shared_shift_idx, get_stop(), shared_afc_on, menu_mode, shared_search_state, shared_stop_auto, shared_baud_auto);
            }
        }
        
        if (new_data_ready) {
            memcpy(local_ts, (void*)shared_fft_ts, sizeof(local_ts));
            memcpy(local_wave, (void*)shared_adc_waveform, sizeof(local_wave));
            memcpy(local_mag_m, (void*)shared_mag_m, sizeof(local_mag_m));
            memcpy(local_mag_s, (void*)shared_mag_s, sizeof(local_mag_s));
            new_data_ready = false;

            float sq=0.0f; for(int i=0; i<FFT_SIZE; i++) sq+=local_ts[i]*local_ts[i];
            shared_signal_db = 10.0f*log10f((sq/FFT_SIZE)+1e-10f) - 20.0f*log10f(shared_agc_gain);

            memcpy(real, local_ts, sizeof(local_ts)); memset(imag, 0, sizeof(imag));
            fft.apply_window(real); fft.compute(real, imag); fft.calc_magnitude(real, imag, mag);

            float pk=-100.0f, sm=0.0f; for(int i=0; i<FFT_SIZE/2; i++) { if(mag[i]>pk) pk=mag[i]; sm+=mag[i]; }
            float avg_noise = sm/(FFT_SIZE/2);
            shared_snr_db = pk - avg_noise;

            float shift = get_shift();
            int m_bin = (int)((shared_actual_freq - shift/2.0f) * FFT_SIZE / SAMPLE_RATE);
            int s_bin = (int)((shared_actual_freq + shift/2.0f) * FFT_SIZE / SAMPLE_RATE);
            int search_r = 12;

            float best_m_mag = -100.0f, best_s_mag = -100.0f;
            int best_m_bin = m_bin, best_s_bin = s_bin;

            for(int i = m_bin - search_r; i <= m_bin + search_r; i++) {
                if (i>0 && i<FFT_SIZE/2 && mag[i] > best_m_mag) { best_m_mag = mag[i]; best_m_bin = i; }
            }
            for(int i = s_bin - search_r; i <= s_bin + search_r; i++) {
                if (i>0 && i<FFT_SIZE/2 && mag[i] > best_s_mag) { best_s_mag = mag[i]; best_s_bin = i; }
            }

            // Relaxed squelch with hysteresis for 75 Baud (using dB differences)
            bool sq_strong = ((best_m_mag - avg_noise) > 4.0f || (best_s_mag - avg_noise) > 4.0f) && (shared_snr_db > tuning_sq_snr);
            bool sq_weak = ((best_m_mag - avg_noise) < 1.5f && (best_s_mag - avg_noise) < 1.5f) || (shared_snr_db < tuning_sq_snr - 2.0f);

            if (shared_signal_db < -65.0f) shared_squelch_open = false;
            else if (sq_strong) shared_squelch_open = true;
            else if (sq_weak) shared_squelch_open = false;

            if (shared_squelch_open && shared_afc_on) {
                if ((best_m_mag - best_s_mag) > 2.0f || (best_s_mag - best_m_mag) > 2.0f) {
                    float found_m_f = best_m_bin * SAMPLE_RATE / (float)FFT_SIZE;
                    float found_s_f = best_s_bin * SAMPLE_RATE / (float)FFT_SIZE;
                    float implied_center = (best_m_mag > best_s_mag) ? (found_m_f + shift/2.0f) : (found_s_f - shift/2.0f);
                    implied_center = std::clamp(implied_center, shared_target_freq - 100.0f, shared_target_freq + 100.0f);
                    shared_actual_freq = shared_actual_freq * 0.8f + implied_center * 0.2f;
                }
            } else if (!shared_afc_on && shared_squelch_open) {
                // Keep current frequency if AFC is OFF but squelch is open
            } else {
                shared_actual_freq = shared_actual_freq * 0.98f + shared_target_freq * 0.02f;
            }

            frame_count++;

            for (int i = 0; i < FFT_SIZE / 2; i++) smooth_mag[i] = smooth_mag[i] * 0.7f + mag[i] * 0.3f;

            // --- Signal Search: find ALL RTTY signals, cycle between them ---
            #define MAX_FOUND_SIGNALS 8
            static struct { float center; float shift; int shift_idx; float score; } found_signals[MAX_FOUND_SIGNALS];
            static int found_count = 0;
            static int found_current = -1; // index of currently selected signal
            static uint32_t last_search_time = 0;

            if (shared_search_request) {
                shared_search_request = false;
                uint32_t now_search = time_us_32();

                // If we have recent results (< 10s) and multiple signals, just cycle
                if (found_count > 1 && found_current >= 0 &&
                    (now_search - last_search_time) < 10000000) {
                    int select = (found_current + 1) % found_count;
                    found_current = select;
                    float new_center = found_signals[select].center;
                    shared_target_freq = new_center;
                    shared_actual_freq = new_center;
                    shared_active_shift = found_signals[select].shift;
                    shared_shift_idx = NUM_SHIFTS; // switch to AUTO
                    shared_search_state = 2;
                    flag_settings_change();
                    printf(">> SEARCH: [%d/%d] center=%.0f shift=%.0f score=%.1f (cycle)\n",
                        select + 1, found_count, new_center, found_signals[select].shift, found_signals[select].score);
                    // Re-trigger autodetect only for params already in AUTO.
                    // Chain STOP after BAUD when both are in AUTO.
                    shared_err_rate = 0.0f;
                    if (shared_baud_auto && shared_stop_auto) {
                        shared_baud_detect_req = true; shared_baud_detect_state = 1;
                        shared_chain_stop_after_baud = true;
                    } else if (shared_baud_auto) {
                        shared_baud_detect_req = true; shared_baud_detect_state = 1;
                    } else if (shared_stop_auto) {
                        shared_stop_detect_req = true; shared_stop_detect_state = 1;
                    }
                    if (shared_inv_auto) {
                        shared_inv_uncertain = false; shared_rtty_inv = false;
                    }
                    search_result_time = time_us_32();
                    last_search_time = now_search;
                    goto search_done;
                }

                float noise_sum = 0.0f;
                for (int i = 1; i < FFT_SIZE/2; i++) noise_sum += smooth_mag[i];
                float noise_avg = noise_sum / (FFT_SIZE/2 - 1);
                float min_snr = 8.0f, max_imbalance = 20.0f;
                int tolerance = 2;
                float min_score = 20.0f;

                // Collect ALL candidate signals (large enough for all shifts)
                static constexpr int MAX_CANDIDATES = 128;
                struct { int lo; int hi; int si; float score; float lo_f; float hi_f; } candidates[MAX_CANDIDATES];
                int num_candidates = 0;

                auto parabolic_refine = [&](int k) -> float {
                    if (k <= 0 || k >= FFT_SIZE/2 - 1) return (float)k;
                    float a = smooth_mag[k-1], b = smooth_mag[k], c = smooth_mag[k+1];
                    float denom = a - 2.0f*b + c;
                    if (fabsf(denom) < 1e-6f) return (float)k;
                    float delta = 0.5f * (a - c) / denom;
                    if (delta < -1.0f) delta = -1.0f;
                    if (delta > 1.0f) delta = 1.0f;
                    return (float)k + delta;
                };

                // Always scan ALL shifts — SEARCH should find any signal
                int scan_idxs[NUM_SHIFTS];
                int scan_count = NUM_SHIFTS;
                for (int i = 0; i < NUM_SHIFTS; i++) scan_idxs[i] = i;

                for (int j = 0; j < scan_count; j++) {
                    int si = scan_idxs[j];
                    int shift_bins = (int)(g_shifts[si] * FFT_SIZE / SAMPLE_RATE);
                    for (int lo = 5; lo < FFT_SIZE/2 - shift_bins - tolerance; lo++) {
                        float lo_mag = smooth_mag[lo];
                        float lo_snr = lo_mag - noise_avg;
                        if (lo_snr < min_snr) continue;
                        if (lo > 0 && smooth_mag[lo-1] > lo_mag) continue;
                        if (lo < FFT_SIZE/2-1 && smooth_mag[lo+1] > lo_mag) continue;
                        for (int d = -tolerance; d <= tolerance; d++) {
                            int hi = lo + shift_bins + d;
                            if (hi < 1 || hi >= FFT_SIZE/2) continue;
                            float hi_mag = smooth_mag[hi];
                            float hi_snr = hi_mag - noise_avg;
                            if (hi_snr < min_snr) continue;
                            // Both peaks must be local maxima
                            if (hi > 0 && smooth_mag[hi-1] > hi_mag) continue;
                            if (hi < FFT_SIZE/2-1 && smooth_mag[hi+1] > hi_mag) continue;
                            float imbalance = fabsf(lo_mag - hi_mag);
                            if (imbalance > max_imbalance) continue;
                            float lo_refined = parabolic_refine(lo);
                            float hi_refined = parabolic_refine(hi);
                            float ideal_bins = g_shifts[si] * FFT_SIZE / (float)SAMPLE_RATE;
                            float actual_bins = hi_refined - lo_refined;
                            float dist_penalty = fabsf(actual_bins - ideal_bins) * 2.5f;
                            float score = lo_snr + hi_snr - imbalance * 0.5f - dist_penalty;
                            if (score > min_score) {
                                if (num_candidates < MAX_CANDIDATES) {
                                    candidates[num_candidates++] = {lo, hi, si, score, lo_refined, hi_refined};
                                } else {
                                    int worst = 0;
                                    for (int w = 1; w < MAX_CANDIDATES; w++)
                                        if (candidates[w].score < candidates[worst].score) worst = w;
                                    if (score > candidates[worst].score)
                                        candidates[worst] = {lo, hi, si, score, lo_refined, hi_refined};
                                }
                            }
                        }
                    }
                }

                // Debug: print top 16 bins by SNR to see what FFT actually sees.
                // Helps diagnose why SEARCH picks the wrong shift.
                {
                    struct { int bin; float snr; } top[16];
                    int top_n = 0;
                    for (int i = 5; i < FFT_SIZE/2 - 1; i++) {
                        float snr = smooth_mag[i] - noise_avg;
                        if (snr < 4.0f) continue;
                        if (smooth_mag[i-1] > smooth_mag[i]) continue;
                        if (smooth_mag[i+1] > smooth_mag[i]) continue;
                        if (top_n < 16) {
                            top[top_n++] = {i, snr};
                        } else {
                            int worst = 0;
                            for (int w = 1; w < 16; w++) if (top[w].snr < top[worst].snr) worst = w;
                            if (snr > top[worst].snr) top[worst] = {i, snr};
                        }
                    }
                    for (int i = 0; i < top_n - 1; i++)
                        for (int j = i + 1; j < top_n; j++)
                            if (top[j].snr > top[i].snr) { auto t = top[i]; top[i] = top[j]; top[j] = t; }
                    printf(">> SEARCH-DBG noise_avg=%.2f top peaks:", (double)noise_avg);
                    for (int i = 0; i < top_n && i < 12; i++) {
                        float freq = top[i].bin * SAMPLE_RATE / (float)FFT_SIZE;
                        printf(" %.0f(b%d,%.1f)", (double)freq, top[i].bin, (double)top[i].snr);
                    }
                    printf("\n");
                }

                // Sort candidates by score descending (best first)
                for (int i = 0; i < num_candidates - 1; i++)
                    for (int j = i + 1; j < num_candidates; j++)
                        if (candidates[j].score > candidates[i].score)
                            std::swap(candidates[i], candidates[j]);

                // Deduplicate: suppress candidates whose peaks are near a higher
                // scoring one. Tolerance scales with shift_bins because wider
                // shifts produce more FSK spectral smearing → more adjacent
                // local maxima per real peak.
                for (int i = 0; i < num_candidates; i++) {
                    if (candidates[i].score < 0) continue;
                    int si_i = candidates[i].si;
                    int shift_bins_i = (int)(g_shifts[si_i] * FFT_SIZE / SAMPLE_RATE);
                    int tol_i = shift_bins_i / 8;
                    if (tol_i < 3) tol_i = 3;
                    for (int j = i + 1; j < num_candidates; j++) {
                        if (candidates[j].score < 0) continue;
                        int tol_j;
                        {
                            int shift_bins_j = (int)(g_shifts[candidates[j].si] * FFT_SIZE / SAMPLE_RATE);
                            tol_j = shift_bins_j / 8;
                            if (tol_j < 3) tol_j = 3;
                        }
                        int tol = tol_i > tol_j ? tol_i : tol_j;
                        bool overlap = (abs(candidates[i].lo - candidates[j].lo) <= tol ||
                                        abs(candidates[i].hi - candidates[j].hi) <= tol ||
                                        abs(candidates[i].lo - candidates[j].hi) <= tol ||
                                        abs(candidates[i].hi - candidates[j].lo) <= tol);
                        if (overlap) candidates[j].score = -1.0f;
                    }
                }

                // 425/450 tie-breaker: prefer 450 when ambiguous
                for (int i = 0; i < num_candidates; i++) {
                    if (candidates[i].score < 0) continue;
                    if (g_shifts[candidates[i].si] == 425.0f) {
                        for (int k = 0; k < num_candidates; k++) {
                            if (candidates[k].score < 0) continue;
                            if (g_shifts[candidates[k].si] == 450.0f &&
                                abs(candidates[i].lo - candidates[k].lo) <= 5 &&
                                candidates[k].score >= candidates[i].score - 2.0f) {
                                candidates[i].score = -1.0f;
                                break;
                            }
                        }
                    }
                }

                // Adaptive threshold: discard candidates scoring < 30% of best
                float best_score = 0;
                for (int i = 0; i < num_candidates; i++)
                    if (candidates[i].score > best_score) best_score = candidates[i].score;
                float score_threshold = best_score * 0.40f;
                for (int i = 0; i < num_candidates; i++)
                    if (candidates[i].score >= 0 && candidates[i].score < score_threshold)
                        candidates[i].score = -1.0f;

                // (dedup debug removed)

                // Collect surviving candidates into found_signals.
                // Use parabolic-refined peak positions for sub-bin center
                // accuracy — raw integer bins give ~10 Hz quantization error.
                found_count = 0;
                for (int i = 0; i < num_candidates && found_count < MAX_FOUND_SIGNALS; i++) {
                    if (candidates[i].score < 0) continue;
                    float lo_hz = candidates[i].lo_f * SAMPLE_RATE / (float)FFT_SIZE;
                    float hi_hz = candidates[i].hi_f * SAMPLE_RATE / (float)FFT_SIZE;
                    found_signals[found_count++] = {(lo_hz+hi_hz)/2.0f, g_shifts[candidates[i].si], candidates[i].si, candidates[i].score};
                }

                // Sort by center frequency (left to right on waterfall)
                for (int i = 0; i < found_count - 1; i++)
                    for (int j = i + 1; j < found_count; j++)
                        if (found_signals[j].center < found_signals[i].center)
                            std::swap(found_signals[i], found_signals[j]);

                if (found_count > 0) {
                    // After a full FFT rescan, always pick by best score (with
                    // wider-shift preference). The cycle-by-frequency path lives
                    // in the early-exit branch above, where candidates are reused
                    // from the previous scan — here they're fresh.
                    int best_score_idx = 0;
                    for (int i = 1; i < found_count; i++)
                        if (found_signals[i].score > found_signals[best_score_idx].score) best_score_idx = i;
                    float best = found_signals[best_score_idx].score;
                    float threshold = best * 0.70f;
                    int select = best_score_idx;
                    for (int i = 0; i < found_count; i++) {
                        if (found_signals[i].score < threshold) continue;
                        if (found_signals[i].shift > found_signals[select].shift)
                            select = i;
                    }
                    found_current = select;

                    float new_center = found_signals[select].center;
                    shared_target_freq = new_center;
                    shared_actual_freq = new_center;
                    // SEARCH always applies the detected shift
                    shared_active_shift = found_signals[select].shift;
                    shared_shift_idx = NUM_SHIFTS; // switch to AUTO
                    shared_search_state = 2;
                    flag_settings_change();
                    printf(">> SEARCH: [%d/%d] center=%.0f shift=%.0f score=%.1f\n",
                        select + 1, found_count, new_center, found_signals[select].shift, found_signals[select].score);
                    if (found_count > 1) {
                        printf(">> Signals found:");
                        for (int i = 0; i < found_count; i++)
                            printf(" %.0f(%s%.0f,s=%.1f)", found_signals[i].center,
                                i == select ? "*" : "", found_signals[i].shift, found_signals[i].score);
                        printf("\n");
                    }
                    // AUTODETECT pipeline: only trigger auto for params already in AUTO.
                    // STOP-DET must run AFTER BAUD-DET, otherwise gap-bin classification
                    // uses stale baud and votes land in the wrong bucket.
                    shared_err_rate = 0.0f;
                    if (shared_baud_auto && shared_stop_auto) {
                        shared_baud_detect_req = true; shared_baud_detect_state = 1;
                        shared_chain_stop_after_baud = true;
                    } else if (shared_baud_auto) {
                        shared_baud_detect_req = true; shared_baud_detect_state = 1;
                    } else if (shared_stop_auto) {
                        shared_stop_detect_req = true; shared_stop_detect_state = 1;
                    }
                    if (shared_inv_auto) {
                        shared_inv_uncertain = false; shared_rtty_inv = false;
                    }
                } else {
                    shared_search_state = 3;
                    found_current = -1;
                    printf(">> SEARCH: no RTTY signal found\n");
                }
                search_result_time = time_us_32();
                last_search_time = time_us_32();
            }
            search_done:;

            if (auto_scale) {
                float peak_db = -100.0f;
                for (int x = 0; x < 480; x++) {
                    int b = (int)(bin_start + x * bin_per_pixel);
                    if (b >= 0 && b < FFT_SIZE/2 && smooth_mag[b] > peak_db) peak_db = smooth_mag[b];
                }
                ui_noise_floor = ui_noise_floor * 0.90f + (std::max(peak_db,-40.0f) - 50.0f) * 0.10f;
                ui_gain = 0.0f;
            }
            
            float hz_px = ((bin_end-bin_start)*(SAMPLE_RATE/(float)FFT_SIZE))/480.0f;
            int shift_px = (int)(get_shift()/hz_px);
            int half_shift = shift_px / 2;
            int m_x = tune_x - half_shift;
            int s_x = tune_x + half_shift;
            
            marker_spr.fillSprite(PAL_BG);
            if (display_mode != 2) {
                marker_spr.drawFastHLine(0, 13, 480, PAL_GRID); 
                marker_spr.fillTriangle(m_x, 13, m_x - 5, 5, m_x + 5, 5, 0x00FFFFU);
                marker_spr.fillTriangle(s_x, 13, s_x - 5, 5, s_x + 5, 5, 0xFFFF00U);
            }
            ili9488_push_colors(0, UI_Y_MARKER, 480, UI_MARKER_H, (uint16_t*)marker_spr.getBuffer());

            if (tuning_lab_active) {
                // Eye diagram in left half of DSP zone
                spectrum.fillSprite(PAL_BG);
                ui.drawEyeDiagram(spectrum, 240, UI_DSP_ZONE_H);
                ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
            } else if (display_mode == 0) {
                // Write normalized magnitudes into circular history buffer (uint8, 0-255)
                uint8_t* hist_ptr = wf_history + wf_offset * 480;
                for (int x = 0; x < 480; x++) {
                    float eb = bin_start + x * bin_per_pixel; int b0 = (int)eb; float db = smooth_mag[b0] * (1.0f-(eb-b0)) + smooth_mag[std::min(b0+1,FFT_SIZE/2-1)] * (eb-b0);
                    float norm = (db + ui_gain - ui_noise_floor) / 50.0f;
                    norm = std::clamp(norm, 0.0f, 1.0f); if (shared_exp_scale) norm *= norm;
                    hist_ptr[x] = (uint8_t)(norm * 255.0f);
                }
                // Render entire waterfall from history via LUT — no sprite needed
                ili9488_push_waterfall_lut(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, wf_history, tune_x, half_shift, wf_offset);
                wf_offset--;
                if (wf_offset < 0) wf_offset = UI_DSP_ZONE_H - 1;
            } else if (display_mode == 1) { 
                spectrum.fillSprite(PAL_BG);
                for (int x = 0; x < 480; x++) {
                    float eb = bin_start + x * bin_per_pixel; int b0 = (int)eb; float db = smooth_mag[b0] * (1.0f-(eb-b0)) + smooth_mag[std::min(b0+1,FFT_SIZE/2-1)] * (eb-b0);
                    float norm = (db + ui_gain - ui_noise_floor) / 50.0f;
                    norm = std::clamp(norm, 0.0f, 1.0f); if (shared_exp_scale) norm *= norm;
                    int h = (int)(norm * UI_DSP_ZONE_H); if (h > 0) spectrum.drawFastVLine(x, UI_DSP_ZONE_H - h, h, PAL_PEAK);
                }
                spectrum.drawFastVLine(m_x, 0, UI_DSP_ZONE_H, 0x00FFFFU);
                spectrum.drawFastVLine(s_x, 0, UI_DSP_ZONE_H, 0xFFFF00U);
                ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
            } else {
                // Lissajous SCOPE with Phosphor Decay effect
                // Bitmask fade: clear LSBs of each channel, shift right, ~0.84x per frame
                // RGB565: RRRRR GGGGGG BBBBB — mask preserves top bits after shift
                uint32_t* buf32 = (uint32_t*)spectrum.getBuffer();
                const int total_words = (480 * 64) / 2; // 2 pixels per uint32
                const uint32_t mask = 0xE7BCE7BCU; // keeps top 3R, top 5G, top 3B per pixel
                for (int i = 0; i < total_words; i++) {
                    uint32_t v = buf32[i];
                    if (v != 0) buf32[i] = (v >> 1) & mask;
                }

                int cx = 240, cy = 32;
                spectrum.drawFastHLine(0, cy, 480, PAL_GRID);
                spectrum.drawFastVLine(cx, 0, 64, PAL_GRID);

                // Lissajous drawing with table lookup instead of sinf/cosf
                for (int x = 0; x < 480; x++) {
                    float m = sqrtf(local_mag_m[x]) * 60.0f;
                    float s = sqrtf(local_mag_s[x]) * 60.0f;
                    float phase = x * 0.4f;
                    int tidx = (int)(phase * (1024.0f / (2.0f * (float)M_PI))) & 1023;
                    float sin_p = sin_table[tidx];
                    float cos_p = cos_table[tidx];
                    int px = cx + (int)(m * sin_p + s * 0.05f * cos_p);
                    int py = cy + (int)(s * cos_p + m * 0.05f * sin_p);
                    spectrum.drawPixel(std::clamp(px, 0, 479), std::clamp(py, 0, 63), PAL_WAVE);
                }
                ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
            }
        }
        
        static uint32_t touch_ignore_until = 0;
        if (loop_start - last_touch > 50000) {
            uint16_t tx, ty; static bool was_touched = false;
            bool is_touched = tft.getTouch(&tx, &ty);
            if (is_touched && time_us_32() > touch_ignore_until) {
                if (ty >= UI_Y_MARKER && ty <= (UI_Y_DSP + UI_DSP_ZONE_H)) {
                    flag_settings_change(); shared_target_freq = (bin_start + (tx / 480.0f) * (bin_end - bin_start)) * (SAMPLE_RATE / (float)FFT_SIZE);
                    shared_actual_freq = shared_target_freq; // SNAP INSTANTLY
                }
                else if (ty > UI_Y_BOTTOM && !was_touched) {
                    int btn_idx = tx / 68; // 480 / 7 approx 68
                    if (btn_idx == 0) {
                        if (baud_popup_active) { baud_popup_active = false; ui.drawRTTY(); }
                        else { baud_popup_active = true; shift_popup_active = false; stop_popup_active = false; ui.drawBaudPopup(shared_baud_idx, shared_baud_auto); }
                    }
                    else if (btn_idx == 1) {
                        if (shift_popup_active) { shift_popup_active = false; ui.drawRTTY(); }
                        else { shift_popup_active = true; baud_popup_active = false; stop_popup_active = false; ui.drawShiftPopup(shared_shift_idx); }
                    }
                    else if (btn_idx == 2) { shared_rtty_inv = false; shared_afc_on = true; shared_search_request = true; shared_search_state = 1; }
                    else if (btn_idx == 3) { flag_settings_change(); shared_afc_on = !shared_afc_on; }
                    else if (btn_idx == 4) {
                        if (stop_popup_active) { stop_popup_active = false; ui.drawRTTY(); }
                        else { stop_popup_active = true; shift_popup_active = false; baud_popup_active = false; ui.drawStopPopup(shared_stop_idx, shared_stop_auto); }
                    }
                    else if (btn_idx == 5) { ui.clearRTTY(); shared_clear_dsp = true; }
                    else if (btn_idx >= 6) {
                        if (tuning_lab_active) {
                            // Tuning Lab -> back to main menu
                            tuning_lab_active = false;
                            menu_mode = true;
                        } else if (diag_screen_active && !menu_mode) {
                            diag_screen_active = false;
                            ui.drawRTTY();
                        } else {
                            menu_mode = !menu_mode;
                            if(menu_mode) diag_screen_active = false; else ui.drawRTTY();
                        }
                    }
                    
                    if (!menu_mode) reset_confirm_mode = false;
                    ui.drawBottomBar(shared_baud_idx, shared_shift_idx, get_stop(), shared_afc_on, menu_mode, shared_search_state, shared_stop_auto, shared_baud_auto);
                    if (menu_mode) ui.drawMenu(auto_scale, display_mode, "DIAG");
                    touch_ignore_until = time_us_32() + 300000;
                }
                else if (ty >= UI_Y_TEXT && ty < UI_Y_BOTTOM) {
                    if (baud_popup_active && !was_touched) {
                        int local_y = ty - UI_Y_TEXT;
                        int col = tx / 160; // 3 columns
                        int row = (local_y - 5) / 75; // 2 rows
                        if (col >= 0 && col < 3 && row >= 0 && row < 2) {
                            int idx = row * 3 + col;
                            if (idx >= 0 && idx <= 4) {
                                flag_settings_change();
                                if (idx < 4) {
                                    shared_baud_idx = idx;
                                    shared_baud_auto = false;
                                    shared_active_baud = bauds[idx];
                                } else {
                                    // AUTO — trigger histogram detection
                                    shared_baud_idx = 4;
                                    shared_baud_auto = true;
                                    shared_baud_detect_req = true;
                                    shared_baud_detect_state = 1;
                                }
                            }
                        }
                        baud_popup_active = false;
                        ui.drawRTTY();
                        ui.drawBottomBar(shared_baud_idx, shared_shift_idx, get_stop(), shared_afc_on, menu_mode, shared_search_state, shared_stop_auto, shared_baud_auto);
                        touch_ignore_until = time_us_32() + 300000;
                    }
                    else if (shift_popup_active && !was_touched) {
                        int local_y = ty - UI_Y_TEXT;
                        int col = tx / 160; // 3 columns
                        int row = (local_y - 5) / 50; // 3 rows, 5px top padding
                        if (col >= 0 && col < 3 && row >= 0 && row < 3) {
                            int idx = row * 3 + col;
                            if (idx >= 0 && idx <= NUM_SHIFTS) {
                                flag_settings_change();
                                shared_shift_idx = idx;
                            }
                        }
                        shift_popup_active = false;
                        ui.drawRTTY();
                        ui.drawBottomBar(shared_baud_idx, shared_shift_idx, get_stop(), shared_afc_on, menu_mode, shared_search_state, shared_stop_auto, shared_baud_auto);
                        touch_ignore_until = time_us_32() + 300000;
                    }
                    else if (stop_popup_active && !was_touched) {
                        int local_y = ty - UI_Y_TEXT;
                        int col = tx / 240; // 2 columns
                        int row = (local_y - 20) / 60; // 2 rows
                        if (col >= 0 && col < 2 && row >= 0 && row < 2) {
                            int idx = row * 2 + col;
                            if (idx >= 0 && idx <= 3) {
                                flag_settings_change();
                                if (idx < 3) {
                                    shared_stop_idx = idx;
                                    shared_stop_auto = false;
                                    shared_active_stop = (idx==0)?1.0f:(idx==1)?1.5f:2.0f;
                                } else {
                                    // AUTO
                                    shared_stop_idx = 3;
                                    shared_stop_auto = true;
                                    shared_stop_detect_req = true;
                                    shared_stop_detect_state = 1;
                                }
                            }
                        }
                        stop_popup_active = false;
                        ui.drawRTTY();
                        ui.drawBottomBar(shared_baud_idx, shared_shift_idx, get_stop(), shared_afc_on, menu_mode, shared_search_state, shared_stop_auto, shared_baud_auto);
                        touch_ignore_until = time_us_32() + 300000;
                    }
                    else if (reset_confirm_mode && !was_touched) {
                        int local_y = ty - UI_Y_TEXT;
                        if (local_y >= 100 && local_y <= 140) {
                            if (tx >= 280 && tx <= 400) { // YES
                                multicore_lockout_start_blocking();
                                uint32_t ints = save_and_disable_interrupts();
                                flash_range_erase(CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
                                flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
                                restore_interrupts(ints);
                                multicore_lockout_end_blocking();
                                watchdog_enable(1, 1);
                                while(1);
                            } else if (tx >= 80 && tx <= 200) { // NO
                                reset_confirm_mode = false;
                                diag_screen_active = true;
                                ui.drawDiagScreen(shared_adc_v, shared_font_mode);
                            }
                        }
                        // Ignore taps outside YES/NO buttons — keep confirm dialog open
                        touch_ignore_until = time_us_32() + 300000;
                    } else if (menu_mode && !was_touched) {
                        int col = tx / (480/4);
                        int row = (ty - UI_Y_TEXT) / (160/3);
                        int btn = row * 4 + col;
                        if (btn == 0) { flag_settings_change(); display_mode = (display_mode + 1) % 3; spectrum.fillSprite(PAL_BG); }
                        else if (btn == 1) { flag_settings_change(); auto_scale = !auto_scale; }
                        else if (btn == 2) { diag_screen_active = true; menu_mode = false; }
                        else if (btn == 3) { tuning_lab_active = true; menu_mode = false; }

                        if (menu_mode && !reset_confirm_mode) ui.drawMenu(auto_scale, display_mode, "DIAG");
                        touch_ignore_until = time_us_32() + 300000;
                        } else if (diag_screen_active && !was_touched) {
                            // Diagnostics screen touch handling (6 buttons, 80px each)
                            int local_y = ty - UI_Y_TEXT;
                            if (local_y > 111) {
                                if (tx < 240) { // FONT toggle: BIG→MED→SMALL→TINY
                                    shared_font_mode = (shared_font_mode + 1) % 4;
                                    const int widths[] = {55, 62, 73, 90}; // BIG:8px, MED:7px, SMALL:6px, TINY:5px
                                    shared_line_width = widths[shared_font_mode];
                                    flag_settings_change();
                                } else { // RST
                                    reset_confirm_mode = true;
                                    diag_screen_active = false;
                                    ui.drawResetConfirm();
                                }
                                if (!reset_confirm_mode) ui.drawDiagScreen(shared_adc_v, shared_font_mode);
                                touch_ignore_until = time_us_32() + 300000;
                            }
                        } else if (tuning_lab_active && !was_touched) {
                            // Tuning Lab touch: 6 cols x 2 rows, 80px each
                            // Row 0 (y=42..83): A- A_val A+ K- K_val K+
                            // Row 1 (y=86..127): SQ- SQ_val SQ+ DUMP --- SAVE
                            int local_y = ty - UI_Y_TEXT;
                            int col = tx / 80;
                            int row = -1;
                            if (local_y >= 42 && local_y < 84) row = 0;
                            else if (local_y >= 86 && local_y < 128) row = 1;

                            if (row == 0) {
                                if (col == 0) { flag_settings_change(); tuning_dpll_alpha -= 0.005f; if (tuning_dpll_alpha < 0.005f) tuning_dpll_alpha = 0.005f; }
                                else if (col == 2) { flag_settings_change(); tuning_dpll_alpha += 0.005f; if (tuning_dpll_alpha > 0.200f) tuning_dpll_alpha = 0.200f; }
                                else if (col == 3) { flag_settings_change(); tuning_lpf_k -= 0.05f; if (tuning_lpf_k < 0.3f) tuning_lpf_k = 0.3f; }
                                else if (col == 5) { flag_settings_change(); tuning_lpf_k += 0.05f; if (tuning_lpf_k > 2.0f) tuning_lpf_k = 2.0f; }
                            } else if (row == 1) {
                                if (col == 0) { flag_settings_change(); tuning_sq_snr -= 1.0f; }
                                else if (col == 2) { flag_settings_change(); tuning_sq_snr += 1.0f; }
                                else if (col == 3) {
                                    // DUMP toggle: enable/disable diagnostic stream
                                    shared_serial_diag = !shared_serial_diag;
                                }
                                else if (col == 5) {
                                    // SAVE settings to flash
                                    AppSettings s;
                                    s.magic = 0xDEADBEEF;
                                    s.baud_idx = shared_baud_idx;
                                    s.shift_idx = shared_shift_idx;
                                    s.stop_idx = shared_stop_idx;
                                    s.rtty_inv = shared_rtty_inv;
                                    s.display_mode = display_mode;
                                    s.exp_scale = shared_exp_scale;
                                    s.auto_scale = auto_scale;
                                    s.filter_k = tuning_lpf_k;
                                    s.sq_snr = tuning_sq_snr;
                                    s.target_freq = shared_target_freq;
                                    s.serial_diag = shared_serial_diag;
                                    s.line_width = shared_line_width;
                                    s.afc_on = shared_afc_on;
                                    s.font_mode = shared_font_mode;
                                    s.dpll_alpha = tuning_dpll_alpha;
                                    s.inv_auto = shared_inv_auto; s.stop_auto = shared_stop_auto;

                                    uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
                                    memcpy(page_buf, &s, sizeof(AppSettings));
                                    multicore_lockout_start_blocking();
                                    uint32_t ints = save_and_disable_interrupts();
                                    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
                                    flash_range_program(SETTINGS_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
                                    restore_interrupts(ints);
                                    multicore_lockout_end_blocking();
                                    settings_need_save = false;
                                    // Show SAVED! feedback
                                    ui.drawTuningControls(tuning_dpll_alpha, tuning_lpf_k, tuning_sq_snr, shared_err_rate, true);
                                    touch_ignore_until = time_us_32() + 500000;
                                    continue; // skip normal redraw below
                                }
                            }
                            ui.drawTuningControls(tuning_dpll_alpha, tuning_lpf_k, tuning_sq_snr, shared_err_rate);
                            touch_ignore_until = time_us_32() + 200000;

 } else if (!menu_mode && !diag_screen_active && !tuning_lab_active) {

                        if (tx > 440) {
                            int local_y = ty - UI_Y_TEXT;
                            if (local_y < 30) ui.scrollRTTY(1);
                            else if (local_y > 130) ui.scrollRTTY(-1);
                            else ui.scrollToY(local_y - 30, 100);
                        } else if (!was_touched && diag_screen_active) {
                            diag_screen_active = false;
                            ui.drawRTTY();
                            touch_ignore_until = time_us_32() + 300000;
                        }
                    }
                }
            }
            was_touched = is_touched; last_touch = loop_start;
        }
        
        uint32_t loop_end = time_us_32();
        c1_total_work += (loop_end - loop_start);
        if (loop_start - c1_last_measure >= 500000) {
            shared_core1_load = (c1_total_work * 100.0f) / (float)(loop_start - c1_last_measure);
            c1_total_work = 0; c1_last_measure = loop_start;
        }

        // Short yield when idle — reduces AHB bus pressure from Core 1
        if (!new_data_ready) sleep_us(20);

        if (settings_need_save && (loop_start - settings_last_change > 15000000)) {
            AppSettings s;
            s.magic = 0xDEADBEEF;
            s.baud_idx = shared_baud_idx;
            s.shift_idx = shared_shift_idx;
            s.stop_idx = shared_stop_idx;
            s.rtty_inv = shared_rtty_inv;
            s.display_mode = display_mode;
            s.exp_scale = shared_exp_scale;
            s.auto_scale = auto_scale;
            s.filter_k = tuning_lpf_k;
            s.sq_snr = tuning_sq_snr;
            s.target_freq = shared_target_freq;
            s.serial_diag = shared_serial_diag;
            s.line_width = shared_line_width;
            s.afc_on = shared_afc_on;
            s.font_mode = shared_font_mode;
            s.dpll_alpha = tuning_dpll_alpha;
            s.inv_auto = shared_inv_auto; s.stop_auto = shared_stop_auto;

            // Skip write if flash already has identical data
            if (memcmp(flash_settings_contents, &s, sizeof(AppSettings)) != 0) {
                uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
                memcpy(page_buf, &s, sizeof(AppSettings));

                multicore_lockout_start_blocking();
                uint32_t ints = save_and_disable_interrupts();
                flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
                flash_range_program(SETTINGS_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
                restore_interrupts(ints);
                multicore_lockout_end_blocking();
            }
            settings_need_save = false;
        }
        tight_loop_contents();
    }
}
