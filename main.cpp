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

// 63-Tap FIR Bandpass Filter (200Hz - 3200Hz, Fs=10000Hz)
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
    int display_mode = 0; // 0=WF, 1=SPEC, 2=OSC
    bool show_palette = false;
    
    int rtty_baud_idx = 0; // 0=45, 1=50, 2=75
    int rtty_shift_idx = 0; // 0=170, 1=200, 2=425, 3=850
    const float shifts[] = {170.0f, 200.0f, 425.0f, 850.0f};
    
    ui.drawBottomBar(auto_scale, exp_scale, menu_mode, display_mode, show_palette, rtty_baud_idx, rtty_shift_idx);
    ui.drawInfo(show_palette);

    LGFX_Sprite spectrum(&tft); spectrum.setColorDepth(16); spectrum.createSprite(480, UI_DSP_ZONE_H);
    LGFX_Sprite marker_spr(&tft); marker_spr.setColorDepth(16); marker_spr.createSprite(480, UI_MARKER_H);
    
    const int bin_start = 5, bin_end = 358;
    const float bin_per_pixel = (float)(bin_end - bin_start) / 480.0f;
    uint32_t last_touch = time_us_32(), last_ui_update = time_us_32(), frame_count = 0;
    float local_mag[FFT_SIZE / 2], local_wave[480];
    int16_t tune_x = 240;
    float ui_noise_floor = -60.0f, ui_gain = 0.0f;
    static float smooth_mag[FFT_SIZE / 2] = {0};

    uint32_t c1_total_work = 0, c1_total_time = 0;
    uint32_t c1_last_measure = time_us_32();

    while (true) {
        uint32_t now = time_us_32();
        uint32_t loop_start = now;

        if (now - last_ui_update > 500000) {
            uint32_t fps = frame_count * 2; frame_count = 0; last_ui_update = now;
            float hz_px = ((bin_end-bin_start)*(SAMPLE_RATE/(float)FFT_SIZE))/480.0f;
            int shift_px = (int)(shifts[rtty_shift_idx]/hz_px);
            int half_shift = shift_px / 2;
            
            float m_freq = (bin_start + ((tune_x - half_shift) / 480.0f) * (bin_end - bin_start)) * (SAMPLE_RATE / (float)FFT_SIZE);
            float s_freq = (bin_start + ((tune_x + half_shift) / 480.0f) * (bin_end - bin_start)) * (SAMPLE_RATE / (float)FFT_SIZE);
            bool is_clipping = shared_adc_clipping; shared_adc_clipping = false; 
            ui.updateTopBar(shared_adc_v, fps, shared_signal_db, shared_snr_db, m_freq, s_freq, is_clipping, shared_core0_load, shared_core1_load);
        }
        
        if (new_data_ready) {
            frame_count++; memcpy(local_mag, (void*)shared_fft_mag, sizeof(local_mag)); memcpy(local_wave, (void*)shared_adc_waveform, sizeof(local_wave)); new_data_ready = false;
            
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
            int shift_px = (int)(shifts[rtty_shift_idx]/hz_px);
            int half_shift = shift_px / 2;
            
            int m_x = tune_x - half_shift;
            int s_x = tune_x + half_shift;
            
            // --- Render Marker Bar independently ---
            marker_spr.fillSprite(PAL_BG);
            marker_spr.drawFastHLine(0, 13, 480, PAL_GRID); // bottom border line
            
            // Triangles pointing down to the DSP zone
            marker_spr.fillTriangle(m_x, 13, m_x - 5, 5, m_x + 5, 5, 0x00FFFFU); // Cyan hex -> Yellow Visual (Space)
            marker_spr.fillTriangle(s_x, 13, s_x - 5, 5, s_x + 5, 5, 0xFFFF00U); // Yellow hex -> Cyan Visual (Mark)
            
            ili9488_push_colors(0, UI_Y_MARKER, 480, UI_MARKER_H, (uint16_t*)marker_spr.getBuffer());
            // ---------------------------------------

            if (display_mode == 0) { // Waterfall
                // Waterfall scrolls on every new frame
                spectrum.scroll(0, 1);
                for (int x = 0; x < 480; x++) {
                    float eb = bin_start + x * bin_per_pixel; int b0 = (int)eb; float db = smooth_mag[b0] * (1.0f-(eb-b0)) + smooth_mag[std::min(b0+1,FFT_SIZE/2-1)] * (eb-b0);
                    float norm = (db + ui_gain - ui_noise_floor) / 50.0f;
                    norm = std::clamp(norm, 0.0f, 1.0f); if (exp_scale) norm *= norm;
                    
                    uint8_t r=0, g=0, b=0;
                    if (norm < 0.25f) { b = (uint8_t)(norm * 4.0f * 255); }
                    else if (norm < 0.5f) { b = 255; g = (uint8_t)((norm - 0.25f) * 4.0f * 255); }
                    else if (norm < 0.75f) { g = 255; r = (uint8_t)((norm - 0.5f) * 4.0f * 255); b = 255 - r; }
                    else { r = 255; g = 255 - (uint8_t)((norm - 0.75f) * 4.0f * 255); }
                    
                    spectrum.drawPixel(x, 0, lgfx::color565(b, g, r)); // Swapped R/B
                }
                
                ili9488_push_waterfall(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer(), tune_x, half_shift);
            } else if (display_mode == 1) { // Spectrum
                spectrum.fillSprite(PAL_BG);
                
                for (int x = 0; x < 480; x++) {
                    float eb = bin_start + x * bin_per_pixel; int b0 = (int)eb; float db = smooth_mag[b0] * (1.0f-(eb-b0)) + smooth_mag[std::min(b0+1,FFT_SIZE/2-1)] * (eb-b0);
                    float norm = (db + ui_gain - ui_noise_floor) / 50.0f;
                    norm = std::clamp(norm, 0.0f, 1.0f); if (exp_scale) norm *= norm;
                    int h = (int)(norm * UI_DSP_ZONE_H); if (h > 0) spectrum.drawFastVLine(x, UI_DSP_ZONE_H - h, h, PAL_PEAK);
                }
                
                spectrum.drawFastVLine(tune_x - half_shift, 0, UI_DSP_ZONE_H, 0x00FFFFU); // Cyan hex
                spectrum.drawFastVLine(tune_x + half_shift, 0, UI_DSP_ZONE_H, 0xFFFF00U); // Yellow hex
                
                ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
            } else if (display_mode == 2) { // Oscilloscope
                spectrum.fillSprite(PAL_BG);
                
                int osc_h = UI_DSP_ZONE_H; 
                spectrum.drawFastHLine(0, osc_h/2, 480, PAL_GRID); 
                for (int x = 0; x < 479; x++) {
                    float ac0 = local_wave[x] - 1.65f;
                    float ac1 = local_wave[x+1] - 1.65f;
                    int y0 = osc_h/2 - (int)((ac0 / 1.65f) * (osc_h/2));
                    int y1 = osc_h/2 - (int)((ac1 / 1.65f) * (osc_h/2));
                    spectrum.drawLine(x, std::clamp(y0,0,osc_h-1), x+1, std::clamp(y1,0,osc_h-1), PAL_WAVE);
                }
                
                spectrum.drawFastVLine(tune_x - half_shift, 0, UI_DSP_ZONE_H, 0x00FFFFU);
                spectrum.drawFastVLine(tune_x + half_shift, 0, UI_DSP_ZONE_H, 0xFFFF00U);
                
                ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
            }
        }
        
        if (now - last_touch > 50000) {
            uint16_t tx, ty; static bool was_touched = false;
            bool is_touched = tft.getTouch(&tx, &ty);
            if (is_touched) {
                // Check if touch is within the combined Marker + DSP zone
                if (ty >= UI_Y_MARKER && ty <= (UI_Y_DSP + UI_DSP_ZONE_H)) {
                    tune_x = tx;
                }
                else if (ty > UI_Y_BOTTOM && !was_touched) {
                    int btn_idx = tx / 80;
                    if (!menu_mode) {
                        // MAIN MENU: BAUD, SHIFT, DISP, EXP/LIN, AUTO, MENU
                        if (btn_idx == 0) { rtty_baud_idx = (rtty_baud_idx + 1) % 3; }
                        else if (btn_idx == 1) { rtty_shift_idx = (rtty_shift_idx + 1) % 4; }
                        else if (btn_idx == 2) { display_mode = (display_mode + 1) % 3; spectrum.fillSprite(PAL_BG); }
                        else if (btn_idx == 3) { exp_scale = !exp_scale; }
                        else if (btn_idx == 4) { auto_scale = true; }
                        else if (btn_idx == 5) { menu_mode = true; } 
                    } else {
                        // SUB MENU: FL-, FL+, GN-, GN+, PAL, BACK
                        if (btn_idx == 0) { ui_noise_floor -= 5.0f; auto_scale = false; }
                        else if (btn_idx == 1) { ui_noise_floor += 5.0f; auto_scale = false; }
                        else if (btn_idx == 2) { ui_gain -= 1.0f; auto_scale = false; }
                        else if (btn_idx == 3) { ui_gain += 1.0f; auto_scale = false; }
                        else if (btn_idx == 4) { show_palette = !show_palette; ui.drawInfo(show_palette); }
                        else if (btn_idx == 5) { menu_mode = false; } 
                    }
                    ui.drawBottomBar(auto_scale, exp_scale, menu_mode, display_mode, show_palette, rtty_baud_idx, rtty_shift_idx);
                }
            }
            was_touched = is_touched; last_touch = now;
        }
        
        uint32_t loop_end = time_us_32();
        c1_total_work += (loop_end - loop_start);
        
        if (now - c1_last_measure >= 500000) {
            shared_core1_load = (c1_total_work * 100.0f) / (now - c1_last_measure);
            c1_total_work = 0; c1_last_measure = now;
        }
        
        tight_loop_contents();
    }
}

void core0_dsp_loop() {
    static SimpleFFT fft; static float ts[FFT_SIZE], real[FFT_SIZE], imag[FFT_SIZE], mag[FFT_SIZE/2], tw[480], fb[63]={0};
    int sc=0, wi=0, fi=0; adc_init(); adc_gpio_init(ADC_PIN); adc_select_input(0);
    float dc=0.0f;
    while(true) {
        uint32_t st = time_us_32(); uint16_t rv = adc_read();
        if(rv<50 || rv>4045) shared_adc_clipping=true;
        float v = (rv/4095.0f)*3.3f; shared_adc_v=v; float s = (rv-2048.0f)/2048.0f;
        dc = dc*0.99f + s*0.01f; s -= dc; fb[fi]=s; float f=0.0f; int bi=fi;
        for(int i=0; i<63; i++) { f += fir_coeffs[i]*fb[bi]; bi--; if(bi<0) bi=62; }
        fi=(fi+1)%63; if(wi<480) tw[wi++]=f*1.65f+1.65f; ts[sc++]=f*2.0f;
        if(sc==FFT_SIZE) {
            float sq=0.0f; for(int i=0; i<FFT_SIZE; i++) sq+=ts[i]*ts[i];
            shared_signal_db = 10.0f*log10f((sq/FFT_SIZE)+1e-10f);
            memcpy(real, ts, sizeof(ts)); memset(imag, 0, sizeof(imag));
            fft.apply_window(real); fft.compute(real, imag); fft.calc_magnitude(real, imag, mag);
            float pk=-100.0f, sm=0.0f; for(int i=0; i<FFT_SIZE/2; i++) { if(mag[i]>pk) pk=mag[i]; sm+=mag[i]; }
            shared_snr_db = pk - (sm/(FFT_SIZE/2)); memcpy((void*)shared_fft_mag, mag, sizeof(mag)); memcpy((void*)shared_adc_waveform, tw, sizeof(tw));
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