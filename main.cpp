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

    // Initialize UI Manager with the new layout
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
    const int shift = 17; // Roughly 170Hz

    // Interactive SDR Controls
    float ui_noise_floor = -50.0f;
    float ui_gain = 40.0f;
    static float smooth_mag[FFT_SIZE / 2] = {0};

    while (true) {
        uint32_t now = time_us_32();

        if (now - last_ui_update > 500000) {
            uint32_t fps = frame_count * 2;
            frame_count = 0;
            last_ui_update = now;

            float marker_freq = (bin_start + (tune_x / 480.0f) * (bin_end - bin_start)) * (SAMPLE_RATE / (float)FFT_SIZE);
            // Update Top Bar via UI Manager
            ui.updateTopBar(shared_adc_v, fps, shared_signal_db, marker_freq);
        }

        if (new_data_ready) {
            frame_count++;
            memcpy(local_mag, (void*)shared_fft_mag, sizeof(local_mag));
            memcpy(local_wave, (void*)shared_adc_waveform, sizeof(local_wave));
            new_data_ready = false;

            spectrum.fillSprite(tft.color565(0, 0, 40)); // Classic deep blue background

            // --- 1. OSCILLOSCOPE (Top 50px of DSP Zone) ---
            int osc_h = 50;
            spectrum.drawFastHLine(0, osc_h/2, 480, tft.color565(0, 50, 100)); // 1.65V center line
            for (int x = 0; x < 479; x++) {
                int y0 = osc_h - (int)((local_wave[x] / 3.3f) * osc_h);
                int y1 = osc_h - (int)((local_wave[x+1] / 3.3f) * osc_h);
                if (y0 < 0) y0 = 0; if (y0 >= osc_h) y0 = osc_h - 1;
                if (y1 < 0) y1 = 0; if (y1 >= osc_h) y1 = osc_h - 1;
                spectrum.drawLine(x, y0, x+1, y1, tft.color565(255, 255, 0)); // Yellow line
            }

            // --- 2. FFT SPECTRUM (Bottom 62px of DSP Zone) ---
            int fft_y_offset = 50;
            int fft_h = 62;
            
            // Temporal Smoothing (removes grass clipping)
            for (int i = 0; i < FFT_SIZE / 2; i++) {
                smooth_mag[i] = smooth_mag[i] * 0.7f + local_mag[i] * 0.3f;
            }
            
            if (auto_scale) {
                float current_max_db = -100.0f;
                for (int x = 0; x < 480; x++) {
                    float exact_bin = bin_start + x * bin_per_pixel;
                    int b0 = (int)exact_bin;
                    if (b0 >= 0 && b0 < (FFT_SIZE / 2)) {
                        if (smooth_mag[b0] > current_max_db) current_max_db = smooth_mag[b0];
                    }
                }
                float target_max = current_max_db;
                if (target_max < -40.0f) target_max = -40.0f;
                ui_noise_floor = ui_noise_floor * 0.90f + (target_max - 50.0f) * 0.10f;
                ui_gain = ui_gain * 0.90f + 50.0f * 0.10f;
            }
            
            for (int x = 0; x < 480; x++) {
                float exact_bin = bin_start + x * bin_per_pixel;
                int b0 = (int)exact_bin;
                int b1 = b0 + 1;
                if (b0 < 0) b0 = 0;
                if (b1 >= FFT_SIZE / 2) b1 = FFT_SIZE / 2 - 1;
                float frac = exact_bin - b0;
                
                // Use smoothed magnitude
                float db = smooth_mag[b0] * (1.0f - frac) + smooth_mag[b1] * frac;
                
                float normalized = (db - ui_noise_floor) / ui_gain;
                if (normalized < 0.0f) normalized = 0.0f;
                if (normalized > 1.0f) normalized = 1.0f;
                
                if (exp_scale) {
                    normalized = normalized * normalized; // Exponentiate to emphasize peaks
                }

                int h = (int)(normalized * fft_h);
                if (h > fft_h) h = fft_h;
                if (h > 0) {
                    spectrum.drawFastVLine(x, fft_y_offset + fft_h - h, h, tft.color565(255, 165, 0)); // Orange peaks
                }
            }

            // Draw Frequency Scale & UI Settings Info
            spectrum.setTextColor(TFT_WHITE);
            spectrum.setTextSize(1);
            spectrum.setCursor(5, fft_y_offset + 5);
            spectrum.print("50 Hz");
            spectrum.setCursor(410, fft_y_offset + 5);
            spectrum.print("3.5 kHz");
            spectrum.setCursor(180, fft_y_offset + 5);
            if (auto_scale) {
                spectrum.printf("AUTO | Floor: %.0fdB | Gain: %.0fdB", ui_noise_floor, ui_gain);
            } else {
                spectrum.printf("MANUAL | Floor: %.0fdB | Gain: %.0fdB", ui_noise_floor, ui_gain);
            }

            // Draw Markers on Spectrum Sprite
            spectrum.drawFastVLine(tune_x, 0, UI_DSP_ZONE_H, TFT_WHITE); // Center
            spectrum.drawFastVLine(tune_x - shift, 0, UI_DSP_ZONE_H, TFT_CYAN); // Space
            spectrum.drawFastVLine(tune_x + shift, 0, UI_DSP_ZONE_H, TFT_YELLOW); // Mark

            ili9488_push_colors(0, UI_Y_DSP, 480, UI_DSP_ZONE_H, (uint16_t*)spectrum.getBuffer());
        }
        
        // A. Poll Touch
        if (now - last_touch > 50000) {
            uint16_t tx, ty;
            if (tft.getTouch(&tx, &ty)) {
                if (ty >= UI_Y_DSP && ty <= (UI_Y_DSP + UI_DSP_ZONE_H)) {
                    tune_x = tx;
                } else if (ty > UI_Y_BOTTOM) {
                    // Button row clicked
                    int btn_idx = tx / (480 / 6);
                    if (btn_idx == 0) { ui_noise_floor -= 5.0f; auto_scale = false; }      // FL- (Lowers floor)
                    else if (btn_idx == 1) { ui_noise_floor += 5.0f; auto_scale = false; } // FL+ (Raises floor)
                    else if (btn_idx == 2) { ui_gain += 5.0f; auto_scale = false; }        // GN- (More gain)
                    else if (btn_idx == 3) { ui_gain -= 5.0f; auto_scale = false; }        // GN+ (Less gain)
                    else if (btn_idx == 4) { auto_scale = true; }
                    else if (btn_idx == 5) { 
                        exp_scale = !exp_scale; 
                        ui_gain = exp_scale ? 25.0f : 50.0f; // Scale button toggles dynamic range between 25dB (steep) and 50dB (normal)
                        auto_scale = false;
                    }
                    
                    if (ui_gain < 5.0f) ui_gain = 5.0f;
                    
                    // Redraw the bottom bar to update the ON/OFF and SCL texts
                    ui.drawBottomBar(auto_scale, exp_scale);
                }
            }
            last_touch = now;
        }

        if (multicore_fifo_rvalid()) (void)multicore_fifo_pop_blocking();
        tight_loop_contents();
    }
}

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

void core0_dsp_loop() {
    static SimpleFFT fft;
    static float time_samples[FFT_SIZE];
    static float real[FFT_SIZE];
    static float imag[FFT_SIZE];
    static float mag[FFT_SIZE / 2];
    static float temp_wave[480]; 
    static float fir_buf[FIR_TAPS] = {0};

    int samples_collected = 0;
    int wave_idx = 0;
    int fir_idx = 0;
    const int SLIDE = 480; // Matches oscilloscope width perfectly!
    
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(0);

    float dc_offset = 0.0f;

    while (true) {
        uint32_t start_time = time_us_32();
        
        uint16_t raw_val = adc_read();
        
        float v = ((float)raw_val / 4095.0f) * 3.3f;
        shared_adc_v = v;

        if (wave_idx < 480) {
            temp_wave[wave_idx] = v;
            wave_idx++;
        }

        float sample = ((float)raw_val - 2048.0f) / 2048.0f;
        
        // DC Blocker (IIR High-Pass Filter)
        dc_offset = dc_offset * 0.99f + sample * 0.01f;
        sample -= dc_offset;
        
        // Digital FIR Bandpass Filter (200Hz - 3200Hz)
        fir_buf[fir_idx] = sample;
        float filtered = 0.0f;
        int buf_idx = fir_idx;
        for (int i = 0; i < FIR_TAPS; i++) {
            filtered += fir_coeffs[i] * fir_buf[buf_idx];
            buf_idx--;
            if (buf_idx < 0) buf_idx = FIR_TAPS - 1;
        }
        fir_idx = (fir_idx + 1) % FIR_TAPS;

        time_samples[samples_collected] = filtered * 2.0f;
        samples_collected++;

        if (samples_collected == FFT_SIZE) {
            // Calculate RMS Signal Power (dBFS)
            float sum_sq = 0.0f;
            for(int i=0; i<FFT_SIZE; i++) sum_sq += time_samples[i]*time_samples[i];
            shared_signal_db = 10.0f * log10f((sum_sq / FFT_SIZE) + 1e-10f);

            memcpy(real, time_samples, sizeof(time_samples));
            memset(imag, 0, sizeof(imag));
            
            fft.apply_window(real);
            fft.compute(real, imag);
            fft.calc_magnitude(real, imag, mag);

            memcpy((void*)shared_fft_mag, mag, sizeof(mag));
            memcpy((void*)shared_adc_waveform, temp_wave, sizeof(temp_wave));
            
            new_data_ready = true;
            wave_idx = 0;

            memmove(time_samples, &time_samples[SLIDE], (FFT_SIZE - SLIDE) * sizeof(float));
            samples_collected = FFT_SIZE - SLIDE;
        }
        
        while (time_us_32() - start_time < 100) {
            tight_loop_contents();
        }
    }
}

void setup_test_tones() {
    // Tone 1: 1000 Hz on GPIO 27 (Physical Pin 32)
    gpio_set_function(27, GPIO_FUNC_PWM);
    uint slice1 = pwm_gpio_to_slice_num(27);
    pwm_set_clkdiv(slice1, 125.0f); // 300MHz / 125 = 2.4MHz
    pwm_set_wrap(slice1, 2400); // 2.4MHz / 2400 = 1000 Hz
    pwm_set_gpio_level(27, 240); // 10% duty to prevent massive ADC clipping
    pwm_set_enabled(slice1, true);

    // Tone 2: 1450 Hz on GPIO 28 (Physical Pin 34)
    gpio_set_function(28, GPIO_FUNC_PWM);
    uint slice2 = pwm_gpio_to_slice_num(28);
    pwm_set_clkdiv(slice2, 125.0f); // 2.4MHz
    pwm_set_wrap(slice2, 1655); // 2.4MHz / 1655 = ~1450 Hz
    pwm_set_gpio_level(28, 165); // 10% duty to prevent massive ADC clipping
    pwm_set_enabled(slice2, true);
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10);
    set_sys_clock_khz(300000, true);
    
    stdio_init_all();
    sleep_ms(2000);
    
    setup_test_tones();

    multicore_launch_core1(core1_main);
    core0_dsp_loop();
    return 0;
}