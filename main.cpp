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
#include "hardware/pwm.h"
#include "LGFX_Config.hpp"
#include "src/display/ili9341_test.h"
#include "src/dsp/fft.hpp"
#include "src/ui/UIManager.hpp"

// --- Hardware Configuration ---
#define ADC_PIN 26
#define SAMPLE_RATE 10000

// --- Shared DSP Data ---
volatile float shared_fft_mag[FFT_SIZE / 2];
volatile float shared_adc_waveform[480]; // Oscilloscope data
volatile bool new_data_ready = false;
volatile float shared_adc_v = 0.0f; // Live ADC voltage
volatile float shared_signal_db = -80.0f; // RMS Signal Power
volatile bool shared_adc_clipping = false; // ADC Overload indicator
volatile float shared_snr_db = 0.0f; // Signal-to-Noise Ratio

void core1_main() {
    printf("Core 1: Starting UI...\n");
    LGFX_RP2350 tft;
    tft.init();
    tft.setRotation(1); 
    
    uint16_t calData[8];
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(10, 10);
    tft.setTextSize(2);
    tft.println("Touch arrows to calibrate");
    tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 15);

    ili9341_init(); 
    ili9341_fill_screen(COLOR_BLACK);

    UIManager ui(&tft);
    ui.init();
    
    bool auto_scale = true;
    bool exp_scale = false;
    
    ui.drawBottomBar(auto_scale, exp_scale);

    LGFX_Sprite spectrum(&tft);
    spectrum.setColorDepth(16);
    spectrum.createSprite(480, UI_DSP_ZONE_H); // 112px tall

    const int bin_start = 5;  // 50 Hz to skip DC
    const int bin_end = 358;   // 3500 Hz
    const float bin_per_pixel = (float)(bin_end - bin_start) / 480.0f;

    uint32_t last_touch = time_us_32();
    uint32_t last_ui_update = time_us_32();
    uint32_t frame_count = 0;
    float local_mag[FFT_SIZE / 2];
    float local_wave[480];
    
    int16_t tune_x = 240;
    float rtty_shift_hz = 170.0f; 

    // Interactive SDR Controls
    float ui_noise_floor = -60.0f;
    float ui_gain = 0.0f; // 0 dB digital gain offset
    static float smooth_mag[FFT_SIZE / 2] = {0};

    while (true) {
        uint32_t now = time_us_32();

        if (now - last_ui_update > 500000) {
            uint32_t fps = frame_count * 2;
            frame_count = 0;
            last_ui_update = now;

            float marker_freq = (bin_start + (tune_x / 480.0f) * (bin_end - bin_start)) * (SAMPLE_RATE / (float)FFT_SIZE);
            bool is_clipping = shared_adc_clipping;
            shared_adc_clipping = false; 
            ui.updateTopBar(shared_adc_v, fps, shared_signal_db, shared_snr_db, marker_freq, is_clipping);
        }

        if (new_data_ready) {
            frame_count++;
            memcpy(local_mag, (void*)shared_fft_mag, sizeof(local_mag));
            memcpy(local_wave, (void*)shared_adc_waveform, sizeof(local_wave));
            new_data_ready = false;

            spectrum.fillSprite(tft.color565(0, 0, 150)); // Bright Blue from previous version

            // --- 1. OSCILLOSCOPE (Top 50px) ---
            int osc_h = 50;
            spectrum.drawFastHLine(0, osc_h/2, 480, tft.color565(0, 200, 255)); // Cyan center line 
            for (int x = 0; x < 479; x++) {
                int y0 = osc_h - (int)((local_wave[x] / 3.3f) * osc_h);
                int y1 = osc_h - (int)((local_wave[x+1] / 3.3f) * osc_h);
                if (y0 < 0) y0 = 0; if (y0 >= osc_h) y0 = osc_h - 1;
                if (y1 < 0) y1 = 0; if (y1 >= osc_h) y1 = osc_h - 1;
                spectrum.drawLine(x, y0, x+1, y1, TFT_WHITE); // White oscilloscope line
            }

            // --- 2. FFT SPECTRUM (Bottom 62px) ---
            int fft_y_offset = 50;
            int fft_h = 62;
            for (int i = 0; i < FFT_SIZE / 2; i++) {
                smooth_mag[i] = smooth_mag[i] * 0.7f + local_mag[i] * 0.3f;
            }
            
            if (auto_scale) {
                float peak_db = -100.0f;
                for (int x = 0; x < 480; x++) {
                    int b = (int)(bin_start + x * bin_per_pixel);
                    if (b >= 0 && b < FFT_SIZE/2 && smooth_mag[b] > peak_db) peak_db = smooth_mag[b];
                }
                if (peak_db < -40.0f) peak_db = -40.0f;
                ui_noise_floor = ui_noise_floor * 0.90f + (peak_db - 50.0f) * 0.10f;
                ui_gain = 0.0f;
            }
            
            for (int x = 0; x < 480; x++) {
                float exact_bin = bin_start + x * bin_per_pixel;
                int b0 = (int)exact_bin; int b1 = b0 + 1;
                if (b0 < 0) b0 = 0; if (b1 >= FFT_SIZE / 2) b1 = FFT_SIZE / 2 - 1;
                float frac = exact_bin - b0;
                float db = smooth_mag[b0] * (1.0f - frac) + smooth_mag[b1] * frac;
                
                float normalized = (db + ui_gain - ui_noise_floor) / 50.0f;
                if (normalized < 0.0f) normalized = 0.0f;
                if (normalized > 1.0f) normalized = 1.0f;
                if (exp_scale) normalized = normalized * normalized;

                int h = (int)(normalized * fft_h);
                if (h > 0) spectrum.drawFastVLine(x, fft_y_offset + fft_h - h, h, TFT_YELLOW); // Yellow peaks
            }

            spectrum.setTextColor(0xFFFFU); spectrum.setTextSize(1);
            spectrum.setCursor(5, fft_y_offset + 5); spectrum.print("50 Hz");
            spectrum.setCursor(410, fft_y_offset + 5); spectrum.print("3.5 kHz");
            spectrum.setCursor(180, fft_y_offset + 5);
            spectrum.printf("%s | Fl:%.0fdB | Gn:%+.0fdB", auto_scale?"AUTO":"MAN", ui_noise_floor, ui_gain);

            float hz_per_px = ((bin_end - bin_start) * (SAMPLE_RATE / (float)FFT_SIZE)) / 480.0f;
            int shift_px = (int)(rtty_shift_hz / hz_per_px);
            int half_shift = shift_px / 2;
            spectrum.drawFastVLine(tune_x, 0, UI_DSP_ZONE_H, 0xFFFFU); 
            spectrum.drawFastVLine(tune_x - half_shift, 0, UI_DSP_ZONE_H, 0x07FFU); 
            spectrum.drawFastVLine(tune_x + half_shift, 0, UI_DSP_ZONE_H, 0xFFE0U); 

            ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
        }
        
        static bool was_touched = false;
        if (now - last_touch > 50000) {
            uint16_t tx, ty;
            bool is_touched = tft.getTouch(&tx, &ty);
            if (is_touched) {
                if (ty >= UI_Y_DSP && ty <= (UI_Y_DSP + UI_DSP_ZONE_H)) {
                    tune_x = tx;
                } else if (ty > UI_Y_BOTTOM) {
                    if (!was_touched) {
                        int btn_idx = tx / (480 / 6);
                        if (btn_idx == 0) { ui_noise_floor -= 5.0f; auto_scale = false; } 
                        else if (btn_idx == 1) { ui_noise_floor += 5.0f; auto_scale = false; }
                        else if (btn_idx == 2) { ui_gain -= 1.0f; auto_scale = false; }
                        else if (btn_idx == 3) { ui_gain += 1.0f; auto_scale = false; }
                        else if (btn_idx == 4) { auto_scale = true; }
                        else if (btn_idx == 5) { exp_scale = !exp_scale; auto_scale = false; }
                        ui.drawBottomBar(auto_scale, exp_scale);
                    }
                }
            }
            was_touched = is_touched;
            last_touch = now;
        }
        if (multicore_fifo_rvalid()) (void)multicore_fifo_pop_blocking();
        tight_loop_contents();
    }
}

#define FIR_TAPS 63
const float fir_coeffs[FIR_TAPS] = {0.000167f, 0.000000f, 0.001438f, 0.000137f, -0.000722f, 0.001740f, -0.000000f, -0.002612f, 0.001612f, -0.000447f, -0.006590f, 0.000000f, -0.001293f, -0.013294f, -0.004198f, -0.002266f, -0.022758f, -0.011889f, -0.002468f, -0.034228f, -0.023822f, -0.000000f, -0.046225f, -0.041377f, 0.009206f, -0.056824f, -0.070578f, 0.038381f, -0.064124f, -0.160830f, 0.247760f, 0.600546f, 0.247760f, -0.160830f, -0.064124f, 0.038381f, -0.070578f, -0.056824f, 0.009206f, -0.041377f, -0.046225f, -0.000000f, -0.023822f, -0.034228f, -0.002468f, -0.011889f, -0.022758f, -0.002266f, -0.004198f, -0.013294f, -0.001293f, 0.000000f, -0.006590f, -0.000447f, 0.001612f, -0.002612f, -0.000000f, 0.001740f, -0.000722f, 0.000137f, 0.001438f, 0.000000f, 0.000167f};

void core0_dsp_loop() {
    static SimpleFFT fft;
    static float time_samples[FFT_SIZE], real[FFT_SIZE], imag[FFT_SIZE], mag[FFT_SIZE/2], temp_wave[480], fir_buf[FIR_TAPS] = {0};
    int samples_collected = 0, wave_idx = 0, fir_idx = 0;
    adc_init(); adc_gpio_init(ADC_PIN); adc_select_input(0);
    float dc_offset = 0.0f;
    while (true) {
        uint32_t start_time = time_us_32();
        uint16_t raw_val = adc_read();
        if (raw_val < 50 || raw_val > 4045) shared_adc_clipping = true;
        float v = ((float)raw_val / 4095.0f) * 3.3f; shared_adc_v = v;
        float sample = ((float)raw_val - 2048.0f) / 2048.0f;
        dc_offset = dc_offset * 0.99f + sample * 0.01f; sample -= dc_offset;
        fir_buf[fir_idx] = sample; float filtered = 0.0f; int buf_idx = fir_idx;
        for (int i = 0; i < FIR_TAPS; i++) { filtered += fir_coeffs[i] * fir_buf[buf_idx]; buf_idx--; if (buf_idx < 0) buf_idx = FIR_TAPS - 1; }
        fir_idx = (fir_idx + 1) % FIR_TAPS;
        if (wave_idx < 480) { temp_wave[wave_idx] = filtered * 1.65f + 1.65f; wave_idx++; }
        time_samples[samples_collected] = filtered * 2.0f; samples_collected++;
        if (samples_collected == FFT_SIZE) {
            float sum_sq = 0.0f; for(int i=0; i<FFT_SIZE; i++) sum_sq += time_samples[i]*time_samples[i];
            shared_signal_db = 10.0f * log10f((sum_sq / FFT_SIZE) + 1e-10f);
            memcpy(real, time_samples, sizeof(time_samples)); memset(imag, 0, sizeof(imag));
            fft.apply_window(real); fft.compute(real, imag); fft.calc_magnitude(real, imag, mag);
            float peak_db = -100.0f, sum_db = 0.0f;
            for(int i=0; i<FFT_SIZE/2; i++) { if (mag[i] > peak_db) peak_db = mag[i]; sum_db += mag[i]; }
            shared_snr_db = peak_db - (sum_db / (FFT_SIZE / 2));
            memcpy((void*)shared_fft_mag, mag, sizeof(mag)); memcpy((void*)shared_adc_waveform, temp_wave, sizeof(temp_wave));
            new_data_ready = true; wave_idx = 0;
            memmove(time_samples, &time_samples[480], (FFT_SIZE - 480) * sizeof(float)); samples_collected = FFT_SIZE - 480;
        }
        while (time_us_32() - start_time < 100) tight_loop_contents();
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10);
    set_sys_clock_khz(300000, true);
    stdio_init_all(); sleep_ms(2000);
    multicore_launch_core1(core1_main);
    core0_dsp_loop();
    return 0;
}