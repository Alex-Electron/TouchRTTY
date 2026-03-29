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

// --- Hardware Configuration ---
#define ADC_PIN 26
#define SAMPLE_RATE 10000

// --- Shared DSP Data ---
volatile float shared_fft_mag[FFT_SIZE / 2];
volatile float shared_adc_waveform[480]; // Oscilloscope data
volatile bool new_data_ready = false;
volatile float shared_adc_v = 0.0f; // Live ADC voltage

// FFT Scope Layout
const int sp_h = 150; 
const int sp_y = 40;

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

    LGFX_Sprite top_bar(&tft);
    top_bar.setColorDepth(16);
    top_bar.createSprite(480, 36);

    LGFX_Sprite text_area(&tft);
    text_area.setColorDepth(16);
    text_area.createSprite(480, 130); 
    
    LGFX_Sprite spectrum(&tft);
    spectrum.setColorDepth(16);
    spectrum.createSprite(480, sp_h);

    const int bin_start = 0;  // 0 Hz
    const int bin_end = 358;   // 3500 Hz (at 10kHz SR, bin resolution is 9.76Hz. 3500/9.76 = 358)
    const float bin_per_pixel = (float)(bin_end - bin_start) / 480.0f;

    uint32_t last_ui_update = time_us_32();
    uint32_t frame_count = 0;
    float local_mag[FFT_SIZE / 2];
    float local_wave[480];

    while (true) {
        uint32_t now = time_us_32();

        if (now - last_ui_update > 500000) {
            uint32_t fps = frame_count * 2;
            frame_count = 0;
            last_ui_update = now;

            top_bar.fillSprite(tft.color565(26, 26, 26));
            top_bar.setTextColor(tft.color565(0, 255, 0));
            top_bar.setTextSize(2);
            top_bar.setCursor(10, 10);
            top_bar.print("ADC TEST");
            top_bar.setTextSize(1);
            top_bar.setTextColor(TFT_WHITE);
            top_bar.setCursor(130, 12);
            top_bar.printf("Live ADC: %.2fV | FPS: %lu", shared_adc_v, fps);
            ili9488_push_colors(0, 0, 480, 36, (uint16_t*)top_bar.getBuffer());

            text_area.fillSprite(TFT_BLACK);
            text_area.setTextColor(tft.color565(0, 255, 0));
            text_area.setTextSize(2);
            text_area.setCursor(5, 5);
            text_area.println("OSCILLOSCOPE & FFT MODE");
            text_area.setTextColor(TFT_WHITE);
            text_area.printf("\nIf you see a green line moving\nand blue peaks jumping,\nthe hardware is PERFECT!\n");
            ili9488_push_colors(0, 190, 480, 130, (uint16_t*)text_area.getBuffer());
        }

        if (new_data_ready) {
            frame_count++;
            memcpy(local_mag, (void*)shared_fft_mag, sizeof(local_mag));
            memcpy(local_wave, (void*)shared_adc_waveform, sizeof(local_wave));
            new_data_ready = false;

            spectrum.fillSprite(tft.color565(0, 0, 40)); // Classic deep blue background

            // --- 1. OSCILLOSCOPE (Top 75px) ---
            int osc_h = 75;
            spectrum.drawFastHLine(0, osc_h/2, 480, tft.color565(0, 50, 100)); // 1.65V center line
            for (int x = 0; x < 479; x++) {
                int y0 = osc_h - (int)((local_wave[x] / 3.3f) * osc_h);
                int y1 = osc_h - (int)((local_wave[x+1] / 3.3f) * osc_h);
                if (y0 < 0) y0 = 0; if (y0 >= osc_h) y0 = osc_h - 1;
                if (y1 < 0) y1 = 0; if (y1 >= osc_h) y1 = osc_h - 1;
                spectrum.drawLine(x, y0, x+1, y1, tft.color565(255, 255, 0)); // Yellow line
            }

            // --- 2. FFT SPECTRUM (Bottom 75px) ---
            int fft_y_offset = 75;
            int fft_h = 75;
            float min_db = 10.0f;
            float max_db = 60.0f;
            
            for (int x = 0; x < 480; x++) {
                float exact_bin = bin_start + x * bin_per_pixel;
                int b0 = (int)exact_bin;
                int b1 = b0 + 1;
                float frac = exact_bin - b0;
                float db = local_mag[b0] * (1.0f - frac) + local_mag[b1] * frac;
                
                float normalized = (db - min_db) / (max_db - min_db);
                if (normalized < 0.0f) normalized = 0.0f;
                if (normalized > 1.0f) normalized = 1.0f;

                int h = (int)(normalized * fft_h);
                if (h > fft_h) h = fft_h;
                if (h > 0) {
                    spectrum.drawFastVLine(x, fft_y_offset + fft_h - h, h, tft.color565(255, 165, 0)); // Orange peaks
                }
            }

            // Draw Frequency Scale
            spectrum.setTextColor(TFT_WHITE);
            spectrum.setTextSize(1);
            spectrum.setCursor(5, fft_y_offset + 5);
            spectrum.print("0 Hz");
            spectrum.setCursor(410, fft_y_offset + 5);
            spectrum.print("3.5 kHz");

            ili9488_push_colors(0, sp_y, 480, sp_h, (uint16_t*)spectrum.getBuffer());
        }
        
        if (multicore_fifo_rvalid()) (void)multicore_fifo_pop_blocking();
        tight_loop_contents();
    }
}

void core0_dsp_loop() {
    static SimpleFFT fft;
    static float real[FFT_SIZE];
    static float imag[FFT_SIZE];
    static float mag[FFT_SIZE / 2];

    int samples_collected = 0;
    int wave_idx = 0;
    const int SLIDE = 333; 
    
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
            shared_adc_waveform[wave_idx] = v;
            wave_idx++;
        }

        float sample = ((float)raw_val - 2048.0f) / 2048.0f;
        
        dc_offset = dc_offset * 0.99f + sample * 0.01f;
        sample -= dc_offset;
        
        real[samples_collected] = sample * 2.0f;
        imag[samples_collected] = 0.0f;
        
        samples_collected++;

        if (samples_collected == FFT_SIZE) {
            fft.apply_window(real);
            fft.compute(real, imag);
            fft.calc_magnitude(real, imag, mag);

            memcpy((void*)shared_fft_mag, mag, sizeof(mag));
            new_data_ready = true;
            wave_idx = 0;

            memmove(real, &real[SLIDE], (FFT_SIZE - SLIDE) * sizeof(float));
            samples_collected = FFT_SIZE - SLIDE;
        }
        
        while (time_us_32() - start_time < 100) {
            tight_loop_contents();
        }
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10);
    set_sys_clock_khz(300000, true);
    
    stdio_init_all();
    sleep_ms(2000);

    multicore_launch_core1(core1_main);
    core0_dsp_loop();
    return 0;
}