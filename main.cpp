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
#include "LGFX_Config.hpp"
#include "src/display/ili9341_test.h"
#include "src/dsp/fft.hpp"
#include "src/dsp/biquad.hpp"
#include "src/ui/UIManager.hpp"
#include "src/version.h"

#define ADC_PIN 26
#define SAMPLE_RATE 10000

volatile float shared_fft_mag[FFT_SIZE / 2];
volatile float shared_adc_waveform[480];
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

volatile float shared_target_freq = 1535.0f;
volatile float shared_actual_freq = 1535.0f;
volatile int shared_baud_idx = 0;
volatile int shared_shift_idx = 0;
volatile bool shared_rtty_inv = false;
volatile bool shared_squelch_open = false;
volatile bool shared_clear_dsp = false;

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
    
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextDatum(middle_center);
    tft.drawString("Touch arrows to calibrate", 240, 160);
    uint16_t calData[8]; 
    tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 15);

    ili9341_init(); 
    ili9341_fill_screen(0x0000);
    UIManager ui(&tft); ui.init();
    
    bool auto_scale = true, exp_scale = true;
    bool menu_mode = false;
    int display_mode = 0;
    bool show_palette = false;
    
    int rtty_stop_idx = 1;
    const float shifts[] = {170.0f, 200.0f, 425.0f, 450.0f, 850.0f};
    const float stop_bits[] = {1.0f, 1.5f, 2.0f};
    
    ui.drawBottomBar(auto_scale, exp_scale, menu_mode, display_mode, shared_baud_idx, shared_shift_idx, stop_bits[rtty_stop_idx], shared_rtty_inv);

    LGFX_Sprite spectrum(&tft); spectrum.setColorDepth(16); spectrum.createSprite(480, UI_DSP_ZONE_H);
    LGFX_Sprite marker_spr(&tft); marker_spr.setColorDepth(16); marker_spr.createSprite(480, UI_MARKER_H);
    
    const int bin_start = 5, bin_end = 358;
    const float bin_per_pixel = (float)(bin_end - bin_start) / 480.0f;
    uint32_t last_touch = time_us_32(), last_ui_update = time_us_32(), frame_count = 0;
    float local_mag[FFT_SIZE / 2], local_wave[480], local_mag_m[480], local_mag_s[480];
    int16_t tune_x = 240;
    float ui_noise_floor = -60.0f, ui_gain = 0.0f;
    static float smooth_mag[FFT_SIZE / 2] = {0};

    uint32_t c1_total_work = 0;
    uint32_t c1_last_measure = time_us_32();

    while (true) {
        uint32_t loop_start = time_us_32();

        if (rtty_char_ready) {
            ui.addRTTYChar((char)rtty_new_char, !show_palette);
            rtty_char_ready = false;
        }

        float bin_idx = shared_actual_freq / (SAMPLE_RATE / (float)FFT_SIZE);
        tune_x = (int)((bin_idx - bin_start) / bin_per_pixel);
        tune_x = std::clamp((int)tune_x, 10, 470);

        if (loop_start - last_ui_update > 500000) {
            uint32_t fps = frame_count * 2; frame_count = 0; last_ui_update = loop_start;
            float m_freq = shared_actual_freq - shifts[shared_shift_idx]/2.0f;
            float s_freq = shared_actual_freq + shifts[shared_shift_idx]/2.0f;
            bool is_clipping = shared_adc_clipping; shared_adc_clipping = false; 
            
            if (show_palette) {
                ui.drawInfo(shared_adc_v);
            }
            
            ui.updateTopBar(shared_adc_v, fps, shared_signal_db, shared_snr_db, m_freq, s_freq, is_clipping, shared_core0_load, shared_core1_load, shared_squelch_open);
        }
        
        if (new_data_ready) {
            frame_count++; 
            memcpy(local_mag, (void*)shared_fft_mag, sizeof(local_mag)); 
            memcpy(local_wave, (void*)shared_adc_waveform, sizeof(local_wave)); 
            memcpy(local_mag_m, (void*)shared_mag_m, sizeof(local_mag_m));
            memcpy(local_mag_s, (void*)shared_mag_s, sizeof(local_mag_s));
            new_data_ready = false;
            
            for (int i = 0; i < FFT_SIZE / 2; i++) smooth_mag[i] = smooth_mag[i] * 0.7f + local_mag[i] * 0.3f;
            
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
            marker_spr.drawFastHLine(0, 13, 480, PAL_GRID); 
            marker_spr.fillTriangle(m_x, 13, m_x - 5, 5, m_x + 5, 5, 0x00FFFFU);
            marker_spr.fillTriangle(s_x, 13, s_x - 5, 5, s_x + 5, 5, 0xFFFF00U);
            ili9488_push_colors(0, UI_Y_MARKER, 480, UI_MARKER_H, (uint16_t*)marker_spr.getBuffer());

            if (display_mode == 0) { 
                spectrum.scroll(0, 1);
                uint16_t* line_ptr = (uint16_t*)spectrum.getBuffer();
                for (int x = 0; x < 480; x++) {
                    float eb = bin_start + x * bin_per_pixel; int b0 = (int)eb; float db = smooth_mag[b0] * (1.0f-(eb-b0)) + smooth_mag[std::min(b0+1,FFT_SIZE/2-1)] * (eb-b0);
                    float norm = (db + ui_gain - ui_noise_floor) / 50.0f;
                    norm = std::clamp(norm, 0.0f, 1.0f); if (exp_scale) norm *= norm;
                    
                    uint8_t r=0, g=0, b=0;
                    if (norm < 0.25f) { b = (uint8_t)(norm * 4.0f * 255.0f); }
                    else if (norm < 0.5f) { b = 255; g = (uint8_t)((norm - 0.25f) * 4.0f * 255.0f); }
                    else if (norm < 0.75f) { g = 255; r = (uint8_t)((norm - 0.5f) * 4.0f * 255.0f); b = 255 - r; }
                    else { r = 255; g = 255 - (uint8_t)((norm - 0.75f) * 4.0f * 255.0f); }
                    
                    line_ptr[x] = lgfx::color565(b, g, r);
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
                spectrum.fillSprite(PAL_BG);
                int cx = 240, cy = 32;
                spectrum.drawFastHLine(0, cy, 480, PAL_GRID); 
                spectrum.drawFastVLine(cx, 0, 64, PAL_GRID); 
                
                for (int x = 0; x < 480; x++) {
                    float m = local_mag_m[x] * 1000.0f; 
                    float s = local_mag_s[x] * 1000.0f;
                    int dx = (int)(m - s);
                    int dy = (int)(m + s);
                    int px = cx + dx;
                    int py = 64 - dy; 
                    spectrum.fillCircle(std::clamp(px, 0, 479), std::clamp(py, 0, 63), 1, PAL_WAVE);
                }
                ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
            }
        }
        
        if (loop_start - last_touch > 50000) {
            uint16_t tx, ty; static bool was_touched = false;
            bool is_touched = tft.getTouch(&tx, &ty);
            if (is_touched) {
                if (ty >= UI_Y_MARKER && ty <= (UI_Y_DSP + UI_DSP_ZONE_H)) {
                    shared_target_freq = (bin_start + (tx / 480.0f) * (bin_end - bin_start)) * (SAMPLE_RATE / (float)FFT_SIZE);
                }
                else if (ty > UI_Y_BOTTOM && !was_touched) {
                    int btn_idx = tx / 80;
                    if (!menu_mode) {
                        if (btn_idx == 0) { shared_baud_idx = (shared_baud_idx + 1) % 3; }
                        else if (btn_idx == 1) { shared_shift_idx = (shared_shift_idx + 1) % 5; }
                        else if (btn_idx == 2) { shared_rtty_inv = !shared_rtty_inv; }
                        else if (btn_idx == 3) { rtty_stop_idx = (rtty_stop_idx + 1) % 3; }
                        else if (btn_idx == 4) { ui.clearRTTY(); shared_clear_dsp = true; }
                        else if (btn_idx == 5) { menu_mode = true; } 
                    } else {
                        if (btn_idx == 0) { display_mode = (display_mode + 1) % 3; spectrum.fillSprite(PAL_BG); }
                        else if (btn_idx == 1) { exp_scale = !exp_scale; }
                        else if (btn_idx == 2) { ui_noise_floor -= 5.0f; auto_scale = false; }
                        else if (btn_idx == 3) { ui_noise_floor += 5.0f; auto_scale = false; }
                        else if (btn_idx == 4) { auto_scale = !auto_scale; }
                        else if (btn_idx == 5) { menu_mode = false; show_palette = false; ui.drawRTTY(); } 
                    }
                    ui.drawBottomBar(auto_scale, exp_scale, menu_mode, display_mode, shared_baud_idx, shared_shift_idx, stop_bits[rtty_stop_idx], shared_rtty_inv);
                }
                else if (ty >= UI_Y_TEXT && ty < UI_Y_BOTTOM && !was_touched) {
                    show_palette = !show_palette;
                    if (show_palette) ui.drawInfo(shared_adc_v);
                    else ui.drawRTTY();
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
        tight_loop_contents();
    }
}

void core0_dsp_loop() {
    for(int i=0; i<1024; i++) {
        sin_table[i] = sinf(2.0f * (float)M_PI * i / 1024.0f);
        cos_table[i] = cosf(2.0f * (float)M_PI * i / 1024.0f);
    }
    
    static SimpleFFT fft; static float ts[FFT_SIZE], real[FFT_SIZE], imag[FFT_SIZE], mag[FFT_SIZE/2], tw[480], tw_m[480], tw_s[480], fb[63]={0};
    int sc=0, wi=0, fi=0; adc_init(); adc_gpio_init(ADC_PIN); adc_select_input(0);
    float dc=0.0f;
    
    float phase_m = 0, phase_s = 0;
    
    Biquad lp_mi, lp_mq, lp_si, lp_sq;
    float current_baud = -1.0f;
    
    float atc_mark_env = 0.01f, atc_space_env = 0.01f;
    
    int baudot_state = 0;
    float symbol_phase = 0.0f;
    float integrate_acc = 0.0f;
    uint8_t current_char = 0;
    bool is_figs = false;
    bool last_d_sign = true;
    
    const float shifts_hz[] = {170.0f, 200.0f, 425.0f, 450.0f, 850.0f};
    const float bauds[] = {45.45f, 50.0f, 75.0f};
    
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
        uint32_t st = time_us_32(); uint16_t rv = adc_read();
        if(rv<50 || rv>4045) shared_adc_clipping=true;
        float v = (rv/4095.0f)*3.3f; shared_adc_v=v; float s = (rv-2048.0f)/2048.0f;
        dc = dc*0.99f + s*0.01f; s -= dc; fb[fi]=s; float f_out=0.0f; int bi=fi;
        for(int i=0; i<63; i++) { f_out += fir_coeffs[i]*fb[bi]; bi--; if(bi<0) bi=62; }
        fi=(fi+1)%63; if(wi<480) { tw[wi] = f_out*1.65f+1.65f; } ts[sc++]=f_out*2.0f;
        
        float baud = bauds[shared_baud_idx];
        if (baud != current_baud) {
            current_baud = baud;
            float fc = baud * 0.75f; 
            design_lpf(&lp_mi, fc, SAMPLE_RATE); design_lpf(&lp_mq, fc, SAMPLE_RATE);
            design_lpf(&lp_si, fc, SAMPLE_RATE); design_lpf(&lp_sq, fc, SAMPLE_RATE);
        }
        
        float shift = shifts_hz[shared_shift_idx];
        float fm = shared_actual_freq - shift / 2.0f;
        float fs = shared_actual_freq + shift / 2.0f;
        
        phase_m += fm / SAMPLE_RATE; if(phase_m >= 1.0f) phase_m -= 1.0f;
        phase_s += fs / SAMPLE_RATE; if(phase_s >= 1.0f) phase_s -= 1.0f;
        int idx_m = (int)(phase_m * 1024) % 1024;
        int idx_s = (int)(phase_s * 1024) % 1024;
        
        float mi = process_biquad(&lp_mi, f_out * cos_table[idx_m]);
        float mq = process_biquad(&lp_mq, f_out * sin_table[idx_m]);
        float si = process_biquad(&lp_si, f_out * cos_table[idx_s]);
        float sq = process_biquad(&lp_sq, f_out * sin_table[idx_s]);
        
        float mark_power = mi*mi + mq*mq;
        float space_power = si*si + sq*sq;
        
        if (wi < 480) { tw_m[wi] = mark_power; tw_s[wi] = space_power; wi++; }
        
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
        
        if (!shared_squelch_open) {
            baudot_state = 0;
            last_d_sign = true; 
        } else {
            if (d_sign != last_d_sign) {
                if (baudot_state > 0) {
                    float err = symbol_phase;
                    if (err > 0.5f) err -= 1.0f;
                    symbol_phase -= err * 0.05f; 
                } else if (!d_sign) { 
                    baudot_state = 1;
                    symbol_phase = 0.0f;
                    integrate_acc = 0.0f;
                    current_char = 0;
                }
            }
            last_d_sign = d_sign;
            
            if (baudot_state > 0) {
                symbol_phase += phase_inc;
                integrate_acc += D; 
                
                if (symbol_phase >= 1.0f) {
                    symbol_phase -= 1.0f;
                    bool bit = (integrate_acc > 0);
                    integrate_acc = 0.0f;
                    
                    if (baudot_state == 1) { 
                        if (bit) baudot_state = 0; 
                        else baudot_state = 2;
                    } else if (baudot_state >= 2 && baudot_state <= 6) { 
                        if (bit) current_char |= (1 << (baudot_state - 2));
                        baudot_state++;
                    } else if (baudot_state == 7) { 
                        if (bit) { 
                            char decoded = '\0';
                            if (current_char == 27) is_figs = true; 
                            else if (current_char == 31) is_figs = false; 
                            else {
                                decoded = is_figs ? ita2_figs[current_char] : ita2_ltrs[current_char];
                                if (decoded == ' ') is_figs = false; 
                                if (decoded != '\0') {
                                    rtty_new_char = decoded;
                                    rtty_char_ready = true;
                                    printf("%c", decoded); 
                                }
                            }
                        }
                        baudot_state = 0; 
                    }
                }
            }
        }
        
        if(sc==FFT_SIZE) {
            float sq=0.0f; for(int i=0; i<FFT_SIZE; i++) sq+=ts[i]*ts[i];
            shared_signal_db = 10.0f*log10f((sq/FFT_SIZE)+1e-10f);
            memcpy(real, ts, sizeof(ts)); memset(imag, 0, sizeof(imag));
            fft.apply_window(real); fft.compute(real, imag); fft.calc_magnitude(real, imag, mag);
            float pk=-100.0f, sm=0.0f; for(int i=0; i<FFT_SIZE/2; i++) { if(mag[i]>pk) pk=mag[i]; sm+=mag[i]; }
            shared_snr_db = pk - (sm/(FFT_SIZE/2)); 
            
            memcpy((void*)shared_fft_mag, mag, sizeof(mag)); 
            memcpy((void*)shared_adc_waveform, tw, sizeof(tw));
            memcpy((void*)shared_mag_m, tw_m, sizeof(tw_m));
            memcpy((void*)shared_mag_s, tw_s, sizeof(tw_s));
            
            int m_bin = (int)((shared_target_freq + shift/2.0f) * FFT_SIZE / SAMPLE_RATE);
            int s_bin = (int)((shared_target_freq - shift/2.0f) * FFT_SIZE / SAMPLE_RATE);
            int search_r = 6; 
            
            float best_m_mag = 0, best_s_mag = 0;
            int best_m_bin = m_bin, best_s_bin = s_bin;
            
            for(int i = m_bin - search_r; i <= m_bin + search_r; i++) {
                if (i>0 && i<FFT_SIZE/2 && mag[i] > best_m_mag) { best_m_mag = mag[i]; best_m_bin = i; }
            }
            for(int i = s_bin - search_r; i <= s_bin + search_r; i++) {
                if (i>0 && i<FFT_SIZE/2 && mag[i] > best_s_mag) { best_s_mag = mag[i]; best_s_bin = i; }
            }
            
            float avg_noise = sm / (FFT_SIZE/2);
            shared_squelch_open = (best_m_mag > avg_noise * 3.0f || best_s_mag > avg_noise * 3.0f) && (shared_snr_db > 2.0f);
            
            if (shared_squelch_open && (best_m_mag > best_s_mag * 1.5f || best_s_mag > best_m_mag * 1.5f)) {
                float found_m_f = best_m_bin * SAMPLE_RATE / (float)FFT_SIZE;
                float found_s_f = best_s_bin * SAMPLE_RATE / (float)FFT_SIZE;
                float implied_center = (best_m_mag > best_s_mag) ? (found_m_f - shift/2.0f) : (found_s_f + shift/2.0f);
                shared_actual_freq = shared_actual_freq * 0.9f + implied_center * 0.1f;
            } else {
                shared_actual_freq = shared_actual_freq * 0.98f + shared_target_freq * 0.02f;
            }
            
            new_data_ready=true; wi=0; memmove(ts, &ts[480], (FFT_SIZE-480)*sizeof(float)); sc=FFT_SIZE-480;
        }
        
        static uint32_t total_work = 0, total_time = 0;
        uint32_t work_end = time_us_32();
        total_work += (work_end - st);
        total_time += 100;
        if (total_time >= 500000) { 
            shared_core0_load = (total_work * 100.0f) / total_time; 
            total_work = 0; total_time = 0; 
        }
        while(time_us_32()-st < 100) tight_loop_contents();
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10); set_sys_clock_khz(300000, true);
    stdio_init_all(); sleep_ms(2000);
    multicore_launch_core1(core1_main); core0_dsp_loop();
    return 0;
}