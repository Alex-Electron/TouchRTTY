#include <stdio.h>
#include <math.h>
#include <algorithm>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/spi.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "LGFX_Config.hpp"
#include "display/ili9488_driver.h"
#include "dsp/fft.hpp"
#include "dsp/biquad.hpp"
#include "dsp/dpll_framer.hpp"
#include "ui/UIManager.hpp"
#include "version.h"

#define ADC_PIN 26
#define SAMPLE_RATE 10000
#define ENC_SW 4

#define CAL_FLASH_OFFSET (1024 * 1024 * 2) // 2MB offset
#define SETTINGS_FLASH_OFFSET (1024 * 1024 * 2 + FLASH_SECTOR_SIZE)
const uint8_t* flash_target_contents = (const uint8_t *) (XIP_BASE + CAL_FLASH_OFFSET);
const uint8_t* flash_settings_contents = (const uint8_t *) (XIP_BASE + SETTINGS_FLASH_OFFSET);

struct AppSettings {
    uint32_t magic;
    int baud_idx;
    int shift_idx;
    int stop_idx;
    bool rtty_inv;
    int display_mode;
    bool exp_scale;
    bool auto_scale;
    float filter_k;
    float sq_snr;
    float target_freq;
    bool serial_diag;
    int line_width;
    bool afc_on;
    int font_mode;
};

volatile bool settings_need_save = false;
volatile uint32_t settings_last_change = 0;
volatile int shared_line_width = 60; 
volatile bool shared_afc_on = true;  
volatile int shared_font_mode = 0; // 0: Font2, 1: Font0 x2

void flag_settings_change() {
    settings_need_save = true;
    settings_last_change = time_us_32();
}

volatile bool shared_force_cal = false;

void load_or_calibrate(lgfx::LGFX_Device& tft, bool force = false) {
    uint16_t calData[8];
    bool valid = true;
    for(int i=0; i<8; i++) {
        calData[i] = ((uint16_t*)flash_target_contents)[i];
        if (calData[i] == 0xFFFF || calData[i] == 0) valid = false;
    }
    
    if (valid && !force) {
        tft.setTouchCalibrate(calData);
    } else {
        tft.fillScreen(0x000000U);
        tft.setTextColor(0xFFFFFFU, 0x000000U);
        tft.setTextSize(2);
        tft.setTextDatum(middle_center);
        tft.drawString("TOUCH 4 CORNERS TO CALIBRATE", 240, 160);
        tft.calibrateTouch(calData, 0xFFFFFFU, 0x000000U, 15);
        
        uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
        memcpy(page_buf, calData, 16);
        
        multicore_lockout_start_blocking();
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
        flash_range_program(CAL_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
        restore_interrupts(ints);
        multicore_lockout_end_blocking();
        
        tft.fillScreen(0x000000U);
    }
}

volatile float shared_fft_mag[FFT_SIZE / 2];
volatile float shared_adc_waveform[480];
volatile float shared_fft_ts[FFT_SIZE];
volatile bool new_data_ready = false;
volatile float shared_adc_v = 0.0f;
volatile float shared_signal_db = -80.0f;
volatile bool shared_adc_clipping = false;
volatile float shared_snr_db = 0.0f;
volatile float shared_core0_load = 0.0f;
volatile float shared_core1_load = 0.0f;

volatile float shared_mag_m[480];
volatile float shared_mag_s[480];

volatile char rtty_new_char = 0;
volatile bool rtty_char_ready = false;
volatile bool shared_err_flag = false;
volatile bool shared_figs_flag = false;
volatile bool shared_ltrs_flag = false;

volatile float shared_diag_adc_min = 4096.0f;
volatile float shared_diag_adc_max = 0.0f;
volatile float shared_atc_m = 0.0f;
volatile float shared_atc_s = 0.0f;
volatile float shared_dpll_phase = 0.0f;
volatile bool shared_diag_ready = false;

volatile float shared_target_freq = 1535.0f;
volatile float shared_actual_freq = 1535.0f;
volatile int shared_baud_idx = 0;
volatile int shared_shift_idx = 0;
volatile int shared_stop_idx = 1;
volatile bool shared_rtty_inv = false;
volatile bool shared_squelch_open = false;
volatile bool shared_clear_dsp = false;

// Tuning Globals
volatile float tuning_dpll_alpha = 0.035f;
volatile float tuning_lpf_k = 0.75f;
volatile float tuning_sq_snr = 6.0f;
volatile float shared_agc_gain = 1.0f;
volatile bool shared_agc_enabled = true;
volatile bool shared_serial_diag = false; // Toggle via Diagnostic Screen

void handle_serial_commands() {
    static char cmd_buf[64];
    static int cmd_ptr = 0;
    int c = getchar_timeout_us(0);
    if (c != PICO_ERROR_TIMEOUT) {
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_ptr] = 0;
            if (cmd_ptr > 0) {
                float val;
                if (sscanf(cmd_buf, "ALPHA %f", &val) == 1) { tuning_dpll_alpha = val; printf(">> SET ALPHA TO %.4f\n", val); }
                else if (sscanf(cmd_buf, "K %f", &val) == 1) { tuning_lpf_k = val; printf(">> SET LPF K TO %.2f\n", val); }
                else if (sscanf(cmd_buf, "SQ %f", &val) == 1) { tuning_sq_snr = val; printf(">> SET SQ SNR TO %.1f\n", val); }
                else if (strcmp(cmd_buf, "CLEAR") == 0) { shared_clear_dsp = true; printf(">> DSP RESET\n"); }
                else if (strcmp(cmd_buf, "INV ON") == 0) { shared_rtty_inv = true; printf(">> RX INVERTED\n"); }
                else if (strcmp(cmd_buf, "INV OFF") == 0) { shared_rtty_inv = false; printf(">> RX NORMAL\n"); }
                else { printf(">> UNKNOWN COMMAND: %s\n", cmd_buf); }
            }
            cmd_ptr = 0;
        } else if (cmd_ptr < 63) {
            cmd_buf[cmd_ptr++] = (char)c;
        }
    }
}

const char ita2_ltrs[32] = {
    '\0', 'E', '\n', 'A', ' ', 'S', 'I', 'U', 
    '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K', 
    'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 
    'O', 'B', 'G', '\0', 'M', 'X', 'V', '\0'
};
const char ita2_figs[32] = {
    '\0', '3', '\n', '-', ' ', '\'', '8', '7', 
    '\r', '$', '4', '\'', ',', '!', ':', '(', 
    '5', '\"', ')', '2', '#', '6', '0', '1', 
    '9', '?', '&', '\0', '.', '/', '=', '\0'
};

typedef struct {
    float gain;
    float target;
    float attack;
    float release;
    float rms;
    float rms_tc;
} agc_t;

inline void agc_init(agc_t *a, float fs) {
    a->gain    = 1.0f;
    a->target  = 0.30f; // Target RMS
    a->attack  = expf(-1.0f / (0.010f * fs)); // 10ms attack
    a->release = expf(-1.0f / (0.500f * fs)); // 500ms release
    a->rms_tc  = expf(-1.0f / (0.050f * fs)); // 50ms RMS window
    a->rms     = 0.01f;
}

inline float agc_process(agc_t *a, float x) {
    float out = x * a->gain;
    a->rms = a->rms * a->rms_tc + out * out * (1.0f - a->rms_tc);
    float rms_now = sqrtf(a->rms + 1e-10f);
    if (rms_now > a->target) {
        a->gain *= a->attack;
    } else {
        a->gain /= a->release;
    }
    a->gain = fmaxf(0.01f, fminf(a->gain, 200.0f));
    return out;
}

float sin_table[1024];
float cos_table[1024];

#define FIR_TAPS 63
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

void core1_main() {
    LGFX_RP2350 tft; tft.init(); tft.setRotation(1);

    bool boot_touch = false;

    if (boot_touch || shared_force_cal) {
        multicore_lockout_start_blocking();
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
        flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
        multicore_lockout_end_blocking();
    }

    load_or_calibrate(tft, boot_touch || shared_force_cal);

    ili9488_init();    ili9488_fill_screen(0x0000);
    UIManager ui(&tft); ui.init();
    
    bool auto_scale = true, exp_scale = true;
    bool menu_mode = false;
    bool diag_screen_active = false;
    int display_mode = 0;
    bool reset_confirm_mode = false;
    uint32_t saved_text_timer = 0;
    
    AppSettings loaded_set;
    memcpy(&loaded_set, flash_settings_contents, sizeof(AppSettings));
    if (loaded_set.magic == 0xDEADBEEF) {
        shared_baud_idx = loaded_set.baud_idx;
        shared_shift_idx = loaded_set.shift_idx;
        shared_stop_idx = loaded_set.stop_idx;
        shared_rtty_inv = loaded_set.rtty_inv;
        display_mode = loaded_set.display_mode;
        exp_scale = loaded_set.exp_scale;
        auto_scale = loaded_set.auto_scale;
        tuning_lpf_k = loaded_set.filter_k;
        tuning_sq_snr = loaded_set.sq_snr;
        shared_target_freq = loaded_set.target_freq;
        shared_actual_freq = shared_target_freq;
        shared_serial_diag = loaded_set.serial_diag; 
        shared_line_width = (loaded_set.line_width >= 30 && loaded_set.line_width <= 80) ? loaded_set.line_width : 60;
        shared_afc_on = loaded_set.afc_on;
        shared_font_mode = loaded_set.font_mode;
    }
    
    const float bauds[] = {45.45f, 50.0f, 75.0f};
    const float shifts[] = {170.0f, 200.0f, 425.0f, 450.0f, 850.0f};
    const float stop_bits[] = {1.0f, 1.5f, 2.0f};
    
    ui.drawBottomBar(shared_baud_idx, shared_shift_idx, stop_bits[shared_stop_idx], shared_rtty_inv, shared_afc_on, menu_mode);

    LGFX_Sprite spectrum(&tft); spectrum.setColorDepth(16); spectrum.createSprite(480, UI_DSP_ZONE_H);
    LGFX_Sprite marker_spr(&tft); marker_spr.setColorDepth(16); marker_spr.createSprite(480, UI_MARKER_H);
    
    const int bin_start = 5, bin_end = 358;
    const float bin_per_pixel = (float)(bin_end - bin_start) / 480.0f;
    uint32_t last_touch = time_us_32(), last_ui_update = time_us_32(), frame_count = 0;
    float local_mag[FFT_SIZE / 2], local_wave[480], local_mag_m[480], local_mag_s[480], local_ts[FFT_SIZE];
    int16_t tune_x = 240;
    float ui_noise_floor = -60.0f, ui_gain = 0.0f;
    static float smooth_mag[FFT_SIZE / 2] = {0};
    
    static SimpleFFT fft; static float real[FFT_SIZE], imag[FFT_SIZE], mag[FFT_SIZE/2];

    uint32_t c1_total_work = 0;
    uint32_t c1_last_measure = time_us_32();

    while (true) {
        uint32_t loop_start = time_us_32();
        handle_serial_commands();

        if (rtty_char_ready) {
            char c = rtty_new_char;
            ui.addRTTYChar(c, !diag_screen_active && !menu_mode);
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

        if (shared_diag_ready && shared_serial_diag) {
            shared_diag_ready = false;
            printf("\n--- TUNING DIAGNOSTICS (B%d) ---\n", BUILD_NUMBER);
            printf("Step 1 (ADC & AGC): V=%.2fV (Range: %.0f-%.0f) AGC_Gain=%.2fx\n", shared_adc_v, shared_diag_adc_min, shared_diag_adc_max, shared_agc_gain);
            printf("Step 2 (ATC Level): Mark_Env=%.4f Space_Env=%.4f\n", shared_atc_m, shared_atc_s);
            printf("Step 3 (FFT Peaks): SNR=%.1f dB, Signal=%.1f dB\n", shared_snr_db, shared_signal_db);
            printf("Step 4 (RTTY Status): Squelch=%s DPLL_Phase=%.2f\n", shared_squelch_open ? "OPEN" : "SHUT", shared_dpll_phase);
            float b = bauds[shared_baud_idx];
            printf("Params: Baud=%.2f ALPHA=%.4f K=%.2f SQ=%.1f\n", b, tuning_dpll_alpha, tuning_lpf_k, tuning_sq_snr);
            printf("---------------------------------\n");
        } else if (shared_diag_ready) {
            shared_diag_ready = false; // Reset anyway if serial diag is off
        }

        float bin_idx = shared_actual_freq / (SAMPLE_RATE / (float)FFT_SIZE);
        tune_x = (int)((bin_idx - bin_start) / bin_per_pixel);
        tune_x = std::clamp((int)tune_x, 10, 470);

        if (loop_start - last_ui_update > 500000) {
            uint32_t fps = frame_count * 2; frame_count = 0; last_ui_update = loop_start;
            float m_freq = shared_actual_freq - shifts[shared_shift_idx]/2.0f;
            float s_freq = shared_actual_freq + shifts[shared_shift_idx]/2.0f;
            bool is_clipping = shared_adc_clipping; shared_adc_clipping = false; 
            
            if (diag_screen_active) {
                ui.drawDiagScreen(shared_adc_v, shared_serial_diag, shared_line_width, shared_font_mode);
            }
            
            ui.updateTopBar(shared_adc_v, fps, shared_signal_db, shared_snr_db, m_freq, s_freq, is_clipping, shared_core0_load, shared_core1_load, shared_squelch_open, shared_agc_gain, shared_agc_enabled);
        }
        
        if (new_data_ready) {
            // ---- PERFORM FFT EXCLUSIVELY ON CORE 1 TO PREVENT CORE 0 TIMING JITTER ----
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
            
            float shift = shifts[shared_shift_idx];
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
            
            if (shared_signal_db < -65.0f) shared_squelch_open = false; // Hard noise floor
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
            
            // --- END FFT/AFC CORE 1 PROCESSING ---
            
            frame_count++;
            
            for (int i = 0; i < FFT_SIZE / 2; i++) smooth_mag[i] = smooth_mag[i] * 0.7f + mag[i] * 0.3f;
            
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
            int shift_px = (int)(shifts[shared_shift_idx]/hz_px);
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

            if (display_mode == 0) { 
                spectrum.scroll(0, 1);
                uint16_t* line_ptr = (uint16_t*)spectrum.getBuffer();
                for (int x = 0; x < 480; x++) {
                    float eb = bin_start + x * bin_per_pixel; int b0 = (int)eb; float db = smooth_mag[b0] * (1.0f-(eb-b0)) + smooth_mag[std::min(b0+1,FFT_SIZE/2-1)] * (eb-b0);
                    float norm = (db + ui_gain - ui_noise_floor) / 50.0f;
                    norm = std::clamp(norm, 0.0f, 1.0f); if (exp_scale) norm *= norm;
                    
                    uint8_t r=0, g=0, b=0; // r=visual_B, g=visual_G, b=visual_R
                    if (norm < 0.25f) { r = (uint8_t)(norm * 4.0f * 255.0f); }
                    else if (norm < 0.5f) { r = 255; g = (uint8_t)((norm - 0.25f) * 4.0f * 255.0f); }
                    else if (norm < 0.75f) { g = 255; b = (uint8_t)((norm - 0.5f) * 4.0f * 255.0f); r = 255 - b; }
                    else { b = 255; g = 255 - (uint8_t)((norm - 0.75f) * 4.0f * 255.0f); }
                    
                    uint16_t c = lgfx::color565(r, g, b); // b goes to blue physically, r goes to red physically
                    line_ptr[x] = (c >> 8) | (c << 8); // Swap for SPI DMA
                }
                ili9488_push_waterfall(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer(), tune_x, half_shift);
            } else if (display_mode == 1) { 
                spectrum.fillSprite(PAL_BG);
                for (int x = 0; x < 480; x++) {
                    float eb = bin_start + x * bin_per_pixel; int b0 = (int)eb; float db = smooth_mag[b0] * (1.0f-(eb-b0)) + smooth_mag[std::min(b0+1,FFT_SIZE/2-1)] * (eb-b0);
                    float norm = (db + ui_gain - ui_noise_floor) / 50.0f;
                    norm = std::clamp(norm, 0.0f, 1.0f); if (exp_scale) norm *= norm;
                    int h = (int)(norm * UI_DSP_ZONE_H); if (h > 0) spectrum.drawFastVLine(x, UI_DSP_ZONE_H - h, h, PAL_PEAK);
                }
                spectrum.drawFastVLine(m_x, 0, UI_DSP_ZONE_H, 0x00FFFFU);
                spectrum.drawFastVLine(s_x, 0, UI_DSP_ZONE_H, 0xFFFF00U);
                ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
            } else { 
                // Lissajous SCOPE with Phosphor Decay effect
                uint16_t* buf_ptr = (uint16_t*)spectrum.getBuffer();
                for (int i = 0; i < 480 * 64; i++) {
                    uint16_t px = buf_ptr[i];
                    if (px != 0) {
                        // Fast RGB565 fade (approx 0.85x)
                        uint16_t r = (px >> 11) & 0x1F;
                        uint16_t g = (px >> 5) & 0x3F;
                        uint16_t b = px & 0x1F;
                        r = (r * 27) >> 5;
                        g = (g * 27) >> 5;
                        b = (b * 27) >> 5;
                        buf_ptr[i] = (r << 11) | (g << 5) | b;
                    }
                }
                
                int cx = 240, cy = 32;
                // Subtle grid
                spectrum.drawFastHLine(0, cy, 480, PAL_GRID); 
                spectrum.drawFastVLine(cx, 0, 64, PAL_GRID); 
                
                for (int x = 0; x < 480; x++) {
                    float m = sqrtf(local_mag_m[x]) * 60.0f; 
                    float s = sqrtf(local_mag_s[x]) * 60.0f;
                    float phase = x * 0.4f;
                    int px = cx + (int)(m * sinf(phase) + s * 0.05f * cosf(phase));
                    int py = cy + (int)(s * cosf(phase) + m * 0.05f * sinf(phase));
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
                    if (btn_idx == 0) { flag_settings_change(); shared_baud_idx = (shared_baud_idx + 1) % 3; }
                    else if (btn_idx == 1) { flag_settings_change(); shared_shift_idx = (shared_shift_idx + 1) % 5; }
                    else if (btn_idx == 2) { flag_settings_change(); shared_rtty_inv = !shared_rtty_inv; }
                    else if (btn_idx == 3) { flag_settings_change(); shared_afc_on = !shared_afc_on; }
                    else if (btn_idx == 4) { flag_settings_change(); shared_stop_idx = (shared_stop_idx + 1) % 3; }
                    else if (btn_idx == 5) { ui.clearRTTY(); shared_clear_dsp = true; }
                    else if (btn_idx >= 6) { 
                        if (diag_screen_active && !menu_mode) {
                            diag_screen_active = false;
                            ui.drawRTTY();
                        } else {
                            menu_mode = !menu_mode;
                            if(menu_mode) diag_screen_active = false; else ui.drawRTTY();
                        }                    } 
                    
                    if (!menu_mode) reset_confirm_mode = false;
                    ui.drawBottomBar(shared_baud_idx, shared_shift_idx, stop_bits[shared_stop_idx], shared_rtty_inv, shared_afc_on, menu_mode);
                    if (menu_mode) ui.drawMenu(auto_scale, exp_scale, display_mode, tuning_lpf_k, tuning_sq_snr, "DIAG", "SAVE");
                    touch_ignore_until = time_us_32() + 300000;
                }
                else if (ty >= UI_Y_TEXT && ty < UI_Y_BOTTOM) {
                    if (reset_confirm_mode && !was_touched) {
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
                            } else { // NO
                                reset_confirm_mode = false;
                                ui.drawMenu(auto_scale, exp_scale, display_mode, tuning_lpf_k, tuning_sq_snr, "DIAG", "SAVE");
                            }
                        } else {
                            reset_confirm_mode = false;
                            ui.drawMenu(auto_scale, exp_scale, display_mode, tuning_lpf_k, tuning_sq_snr, "DIAG", "SAVE");
                        }
                        touch_ignore_until = time_us_32() + 300000;
                    } else if (menu_mode && !was_touched) {
                        int col = tx / (480/4);
                        int row = (ty - UI_Y_TEXT) / (160/3);
                        int btn = row * 4 + col;
                        const char* save_text = "SAVE";
                        if (btn == 0) { flag_settings_change(); display_mode = (display_mode + 1) % 3; spectrum.fillSprite(PAL_BG); }
                        else if (btn == 1) { flag_settings_change(); exp_scale = !exp_scale; }
                        else if (btn == 2) { flag_settings_change(); auto_scale = !auto_scale; }
                        else if (btn == 3) { diag_screen_active = true; menu_mode = false; } // Enter sub-menu
                        else if (btn == 4) { flag_settings_change(); tuning_lpf_k -= 0.05f; if(tuning_lpf_k < 0.5f) tuning_lpf_k = 0.5f; }
                        else if (btn == 6) { flag_settings_change(); tuning_lpf_k += 0.05f; if(tuning_lpf_k > 1.5f) tuning_lpf_k = 1.5f; }
                        else if (btn == 7) {
                           AppSettings s;
                           s.magic = 0xDEADBEEF;
                           s.baud_idx = shared_baud_idx;
                           s.shift_idx = shared_shift_idx;
                           s.stop_idx = shared_stop_idx;
                           s.rtty_inv = shared_rtty_inv;
                           s.display_mode = display_mode;
                           s.exp_scale = exp_scale;
                           s.auto_scale = auto_scale;
                           s.filter_k = tuning_lpf_k;
                           s.sq_snr = tuning_sq_snr;
                           s.target_freq = shared_target_freq;
                           s.serial_diag = shared_serial_diag;
                           s.line_width = shared_line_width;
                           s.afc_on = shared_afc_on;
                           s.font_mode = shared_font_mode; // New

                           uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
                           memcpy(page_buf, &s, sizeof(AppSettings));
                           multicore_lockout_start_blocking();
                           uint32_t ints = save_and_disable_interrupts();
                           flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
                           flash_range_program(SETTINGS_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
                           restore_interrupts(ints);
                           multicore_lockout_end_blocking();

                           save_text = "SAVED!";
                           saved_text_timer = time_us_32();
                           settings_need_save = false;
                        }
                        else if (btn == 8) { flag_settings_change(); tuning_sq_snr -= 1.0f; }
                        else if (btn == 10) { flag_settings_change(); tuning_sq_snr += 1.0f; }
                        else if (btn == 11) { reset_confirm_mode = true; ui.drawResetConfirm(); }

                        if (menu_mode && !reset_confirm_mode) ui.drawMenu(auto_scale, exp_scale, display_mode, tuning_lpf_k, tuning_sq_snr, "DIAG", save_text);
                        touch_ignore_until = time_us_32() + 300000;
                        } else if (diag_screen_active && !was_touched) {
                            // Diagnostics screen touch handling (6 buttons, 80px each)
                            int local_y = ty - UI_Y_TEXT;
                            if (local_y > 111) { 
                                int b_idx = tx / 80;
                                if (b_idx == 0) { // DIAG toggle
                                    shared_serial_diag = !shared_serial_diag;
                                } else if (b_idx == 1) { // FONT toggle
                                    shared_font_mode = (shared_font_mode + 1) % 2;
                                    flag_settings_change();
                                } else if (b_idx == 2) { // WIDTH -
                                    flag_settings_change();
                                    shared_line_width -= 2; if(shared_line_width < 30) shared_line_width = 30;
                                } else if (b_idx == 3) { // WIDTH Reset
                                    flag_settings_change();
                                    shared_line_width = 60;
                                } else if (b_idx == 4) { // WIDTH +
                                    flag_settings_change();
                                    shared_line_width += 2; if(shared_line_width > 85) shared_line_width = 85;
                                } else if (b_idx == 5) { // RST
                                    reset_confirm_mode = true;
                                    diag_screen_active = false;
                                    ui.drawResetConfirm();
                                }
                                if (!reset_confirm_mode) ui.drawDiagScreen(shared_adc_v, shared_serial_diag, shared_line_width, shared_font_mode);
                                touch_ignore_until = time_us_32() + 300000;
                            }
                        }

 else if (!menu_mode && !diag_screen_active) {

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
            shared_core1_load = (c1_total_work * 100.0f) / (loop_start - c1_last_measure);
            c1_total_work = 0; c1_last_measure = loop_start;
        }
        
        if (settings_need_save && (loop_start - settings_last_change > 15000000)) {
            AppSettings s;
            s.magic = 0xDEADBEEF;
            s.baud_idx = shared_baud_idx;
            s.shift_idx = shared_shift_idx;
            s.stop_idx = shared_stop_idx;
            s.rtty_inv = shared_rtty_inv;
            s.display_mode = display_mode;
            s.exp_scale = exp_scale;
            s.auto_scale = auto_scale;
            s.filter_k = tuning_lpf_k;
            s.sq_snr = tuning_sq_snr;
            s.target_freq = shared_target_freq;
            
            uint8_t page_buf[FLASH_PAGE_SIZE] = {0};
            memcpy(page_buf, &s, sizeof(AppSettings));
            
            multicore_lockout_start_blocking();
            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
            flash_range_program(SETTINGS_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
            restore_interrupts(ints);
            multicore_lockout_end_blocking();
            
            settings_need_save = false;
        }
        tight_loop_contents();
    }
}

void core0_dsp_loop() {
    multicore_lockout_victim_init();
    for(int i=0; i<1024; i++) {
        sin_table[i] = sinf(2.0f * (float)M_PI * i / 1024.0f);
        cos_table[i] = cosf(2.0f * (float)M_PI * i / 1024.0f);
    }
    
    static float ts[FFT_SIZE], tw[480], tw_m[480], tw_s[480], fb[63]={0};
    int sc=0, wi=0, fi=0; adc_init(); adc_gpio_init(ADC_PIN); adc_select_input(0);
    float dc=0.0f;
    
    float phase_m = 0, phase_s = 0;
    Biquad lp_mi, lp_mq, lp_si, lp_sq;
    float current_baud = -1.0f;
    float atc_mark_env = 0.01f, atc_space_env = 0.01f;
    
    agc_t agc;
    agc_init(&agc, SAMPLE_RATE);
    
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
    
    const float shifts_hz[] = {170.0f, 200.0f, 425.0f, 450.0f, 850.0f};
    const float bauds[] = {45.45f, 50.0f, 75.0f};
    
    uint32_t next_sample_time = time_us_32();

    while(true) {
        if (shared_clear_dsp) {
            shared_clear_dsp = false;
            shared_actual_freq = shared_target_freq;
            baudot_state = 0;
            symbol_phase = 0.0f;
            integrate_acc = 0.0f;
            atc_mark_env = 0.01f;
            atc_space_env = 0.01f;
            shared_squelch_open = false;
            last_d_sign = true;
        }

        uint32_t st = time_us_32(); 
        uint16_t rv = adc_read();
        
        if (rv < diag_adc_min) diag_adc_min = rv;
        if (rv > diag_adc_max) diag_adc_max = rv;
        
        if(rv<50 || rv>4045) shared_adc_clipping=true;
        float v = (rv/4095.0f)*3.3f; shared_adc_v=v; float s = (rv-2048.0f)/2048.0f;
        dc = dc*0.99f + s*0.01f; s -= dc; fb[fi]=s; float f_out=0.0f; int bi=fi;
        for(int i=0; i<63; i++) { f_out += fir_coeffs[i]*fb[bi]; bi--; if(bi<0) bi=62; }
        fi=(fi+1)%63; 
        
        float agc_out = agc_process(&agc, f_out);
        shared_agc_gain = agc.gain;
        
        if(wi<480) { tw[wi] = agc_out*1.65f+1.65f; } 
        ts[sc++]=agc_out*2.0f;
        
        f_out = agc_out;
        
        float baud = bauds[shared_baud_idx];
        static float current_k = -1.0f;
        float stop_bits_expected = (shared_stop_idx == 0) ? 1.0f : ((shared_stop_idx == 1) ? 1.5f : 2.0f);
        
        if (baud != current_baud || tuning_lpf_k != current_k) {
            current_baud = baud;
            current_k = tuning_lpf_k;
            float fc = baud * tuning_lpf_k; 
            design_lpf(&lp_mi, fc, SAMPLE_RATE); design_lpf(&lp_mq, fc, SAMPLE_RATE);
            design_lpf(&lp_si, fc, SAMPLE_RATE); design_lpf(&lp_sq, fc, SAMPLE_RATE);
        }
        
        float shift = shifts_hz[shared_shift_idx];
        float fm = shared_actual_freq - shift / 2.0f;
        float fs = shared_actual_freq + shift / 2.0f;
        
        phase_m += fm * 0.0001f; if(phase_m >= 1.0f) phase_m -= 1.0f;
        phase_s += fs * 0.0001f; if(phase_s >= 1.0f) phase_s -= 1.0f;
        int idx_m = (int)(phase_m * 1024) % 1024;
        int idx_s = (int)(phase_s * 1024) % 1024;
        
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
        float atc_fast = expf(-1.0f / (2.0f * (SAMPLE_RATE / baud)));
        float atc_slow = expf(-1.0f / (16.0f * (SAMPLE_RATE / baud)));
        
        atc_mark_env = atc_mark_env * (new_m > atc_mark_env ? atc_fast : atc_slow) + new_m * (1.0f - (new_m > atc_mark_env ? atc_fast : atc_slow));
        atc_space_env = atc_space_env * (new_s > atc_space_env ? atc_fast : atc_slow) + new_s * (1.0f - (new_s > atc_space_env ? atc_fast : atc_slow));
        
        float m_norm = new_m / (atc_mark_env + 1e-6f);
        float s_norm = new_s / (atc_space_env + 1e-6f);
        
        float D = m_norm - s_norm;
        D = fmaxf(-1.5f, fminf(1.5f, D));
        if (shared_rtty_inv) D = -D;
        bool d_sign = (D > 0);
        
        float phase_inc = baud / SAMPLE_RATE;
        static float freq_error = 0.0f;
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
                    baudot_state = 1;
                    symbol_phase = 0.0f; 
                    integrate_acc = 0.0f;
                    current_char = 0;
                }
            }
            last_d_sign = d_sign;
            
            if (baudot_state > 0) {
                symbol_phase += phase_inc + freq_error; // Add the accumulated frequency error!
                integrate_acc += D; 
                
                if (symbol_phase >= 1.0f) {
                    symbol_phase -= 1.0f;
                    bool bit = (integrate_acc > 0);
                    integrate_acc = 0.0f;
                    
                    if (baudot_state == 1) { 
                        if (bit) baudot_state = 0; // False start
                        else baudot_state = 2;
                    } else if (baudot_state >= 2 && baudot_state <= 6) { 
                        if (bit) current_char |= (1 << (baudot_state - 2));
                        baudot_state++;
                    } else if (baudot_state == 7) { 
                        if (bit) { // Valid stop
                            char decoded = '\0';
                            if (current_char == 27) { is_figs = true; shared_figs_flag = true; }
                            else if (current_char == 31) { is_figs = false; shared_ltrs_flag = true; }
                            else {
                                decoded = is_figs ? ita2_figs[current_char] : ita2_ltrs[current_char];
                                if (decoded == ' ') is_figs = false; // Unshift on space
                                if (decoded != '\0') {
                                    rtty_new_char = decoded;
                                    rtty_char_ready = true;
                                }
                            }
                        } else {
                            shared_err_flag = true; // Framing error
                        }
                        
                        // For tight 1.0 stop bits, if we are ALREADY in Space, a new start bit has begun!
                        if (stop_bits_expected <= 1.0f && !d_sign) {
                            baudot_state = 1;
                            symbol_phase = 0.0f; // Exact start of bit!
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
            // PASS TO CORE 1 AND DO NOT BLOCK HERE
            memcpy((void*)shared_fft_ts, ts, sizeof(ts)); 
            memcpy((void*)shared_adc_waveform, tw, sizeof(tw));
            memcpy((void*)shared_mag_m, tw_m, sizeof(tw_m));
            memcpy((void*)shared_mag_s, tw_s, sizeof(tw_s));
            new_data_ready=true; 
            
            wi=0; memmove(ts, &ts[480], (FFT_SIZE-480)*sizeof(float)); sc=FFT_SIZE-480;
            
            static int diag_timer = 0;
            if (++diag_timer >= 10) { // Approx 500ms
                diag_timer = 0;
                shared_diag_adc_min = diag_adc_min;
                shared_diag_adc_max = diag_adc_max;
                shared_atc_m = atc_mark_env;
                shared_atc_s = atc_space_env;
                shared_dpll_phase = symbol_phase;
                shared_diag_ready = true;
                
                diag_adc_min = 4096.0f; diag_adc_max = 0.0f;
                diag_m_pow = 0.0f; diag_s_pow = 0.0f;
                diag_samples = 0;
            }
        }
        
        static uint32_t total_work = 0, total_time = 0;
        uint32_t work_end = time_us_32();
        total_work += (work_end - st);
        total_time += 100;
        if (total_time >= 500000) { 
            shared_core0_load = (total_work * 100.0f) / total_time; 
            total_work = 0; total_time = 0; 
        }
        next_sample_time += 100;
        while(time_us_32() < next_sample_time) tight_loop_contents();
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10); set_sys_clock_khz(300000, true);
    
    gpio_init(ENC_SW);
    gpio_set_dir(ENC_SW, GPIO_IN);
    gpio_pull_up(ENC_SW);
    sleep_ms(10);
    
    // 100ms Debounce for Factory Reset check
    bool pressed = true;
    for(int i=0; i<10; i++) {
        if (gpio_get(ENC_SW) != 0) { pressed = false; break; }
        sleep_ms(10);
    }
    shared_force_cal = pressed;
    
    // Hardware Hard Reset (Hold for 3 seconds)
    if (shared_force_cal) {
        gpio_init(PICO_DEFAULT_LED_PIN);
        gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
        for(int i=0; i<15; i++) {
            gpio_put(PICO_DEFAULT_LED_PIN, i%2);
            sleep_ms(200);
            if (gpio_get(ENC_SW) != 0) { shared_force_cal = false; break; }
        }
        if (shared_force_cal) {
            // WIPE IT ALL
            multicore_lockout_start_blocking();
            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
            flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
            restore_interrupts(ints);
            multicore_lockout_end_blocking();
            
            // Flash LED rapidly to confirm
            for(int i=0; i<20; i++) { gpio_put(PICO_DEFAULT_LED_PIN, i%2); sleep_ms(50); }
        }
    }
    
    stdio_init_all(); sleep_ms(2000);
    multicore_launch_core1(core1_main); core0_dsp_loop();
    return 0;
}