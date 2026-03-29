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
    
    bool auto_scale = true, exp_scale = false;
    int active_palette = 1;
    
    ui.drawBottomBar(auto_scale, exp_scale);
    ui.drawPaletteInfo(active_palette);

    LGFX_Sprite spectrum(&tft); spectrum.setColorDepth(16); spectrum.createSprite(480, UI_DSP_ZONE_H);
    const int bin_start = 5, bin_end = 358;
    const float bin_per_pixel = (float)(bin_end - bin_start) / 480.0f;
    uint32_t last_touch = time_us_32(), last_ui_update = time_us_32(), frame_count = 0;
    float local_mag[FFT_SIZE / 2], local_wave[480];
    int16_t tune_x = 240;
    float rtty_shift_hz = 170.0f;
    float ui_noise_floor = -60.0f, ui_gain = 0.0f;
    static float smooth_mag[FFT_SIZE / 2] = {0};

    while (true) {
        uint32_t now = time_us_32();
        int c = getchar_timeout_us(0);
        if (c >= '0' && c <= '3') {
            active_palette = c - '0';
            ui.drawPaletteInfo(active_palette);
            printf("Palette switched to %d: %s\n", active_palette, PALETTES[active_palette].name);
        }

        if (now - last_ui_update > 500000) {
            uint32_t fps = frame_count * 2; frame_count = 0; last_ui_update = now;
            float marker_freq = (bin_start + (tune_x / 480.0f) * (bin_end - bin_start)) * (SAMPLE_RATE / (float)FFT_SIZE);
            bool is_clipping = shared_adc_clipping; shared_adc_clipping = false; 
            ui.updateTopBar(shared_adc_v, fps, shared_signal_db, shared_snr_db, marker_freq, is_clipping);
        }
        
        if (new_data_ready) {
            frame_count++; memcpy(local_mag, (void*)shared_fft_mag, sizeof(local_mag)); memcpy(local_wave, (void*)shared_adc_waveform, sizeof(local_wave)); new_data_ready = false;
            
            const Palette* pal = &PALETTES[active_palette];
            spectrum.fillSprite(pal->bg);
            
            int osc_h = 50; spectrum.drawFastHLine(0, osc_h/2, 480, pal->grid); 
            for (int x = 0; x < 479; x++) {
                int y0 = osc_h - (int)((local_wave[x] / 3.3f) * osc_h);
                int y1 = osc_h - (int)((local_wave[x+1] / 3.3f) * osc_h);
                spectrum.drawLine(x, std::clamp(y0,0,osc_h-1), x+1, std::clamp(y1,0,osc_h-1), pal->wave);
            }
            
            int fft_y_offset = 50, fft_h = 62;
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
            
            for (int x = 0; x < 480; x++) {
                float eb = bin_start + x * bin_per_pixel; int b0 = (int)eb; float db = smooth_mag[b0] * (1.0f-(eb-b0)) + smooth_mag[std::min(b0+1,FFT_SIZE/2-1)] * (eb-b0);
                float norm = (db + ui_gain - ui_noise_floor) / 50.0f;
                norm = std::clamp(norm, 0.0f, 1.0f); if (exp_scale) norm *= norm;
                int h = (int)(norm * fft_h); if (h > 0) spectrum.drawFastVLine(x, fft_y_offset + fft_h - h, h, pal->peak);
            }
            
            spectrum.setTextColor(pal->text); spectrum.setTextSize(1);
            spectrum.setCursor(5, fft_y_offset+5); spectrum.print("50 Hz");
            spectrum.setCursor(410, fft_y_offset+5); spectrum.print("3.5 kHz");
            spectrum.setCursor(180, fft_y_offset+5);
            spectrum.printf("%s|Fl:%.0fdB|Gn:%+.0fdB", auto_scale?"AUTO":"MAN", ui_noise_floor, ui_gain);
            
            float hz_px = ((bin_end-bin_start)*(SAMPLE_RATE/(float)FFT_SIZE))/480.0f;
            int shift_px = (int)(rtty_shift_hz/hz_px);
            int half_shift = shift_px / 2;
            spectrum.drawFastVLine(tune_x, 0, UI_DSP_ZONE_H, 0xFFFFU);
            spectrum.drawFastVLine(tune_x - half_shift, 0, UI_DSP_ZONE_H, 0x07FFU);
            spectrum.drawFastVLine(tune_x + half_shift, 0, UI_DSP_ZONE_H, 0xFFE0U);
            
            ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
        }
        
        static bool was_touched = false;
        if (now - last_touch > 50000) {
            uint16_t tx, ty; bool is_touched = tft.getTouch(&tx, &ty);
            if (is_touched) {
                if (ty >= UI_Y_DSP && ty <= (UI_Y_DSP + UI_DSP_ZONE_H)) tune_x = tx;
                else if (ty > UI_Y_BOTTOM && !was_touched) {
                    int idx = tx / 80;
                    if (idx == 0) { ui_noise_floor -= 5.0f; auto_scale = false; }
                    else if (idx == 1) { ui_noise_floor += 5.0f; auto_scale = false; }
                    else if (idx == 2) { ui_gain -= 1.0f; auto_scale = false; }
                    else if (idx == 3) { ui_gain += 1.0f; auto_scale = false; }
                    else if (idx == 4) { auto_scale = true; }
                    else if (idx == 5) { 
                        active_palette = (active_palette + 1) % 4; 
                        ui.drawPaletteInfo(active_palette);
                        printf("Palette switched via touch to %d\n", active_palette);
                    } 
                    ui.drawBottomBar(auto_scale, exp_scale);
                }
            }
            was_touched = is_touched; last_touch = now;
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
        while(time_us_32()-st < 100) tight_loop_contents();
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10); set_sys_clock_khz(300000, true);
    stdio_init_all(); sleep_ms(2000);
    multicore_launch_core1(core1_main); core0_dsp_loop();
    return 0;
}