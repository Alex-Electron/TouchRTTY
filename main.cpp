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
volatile bool new_fft_ready = false;
volatile float shared_adc_v = 0.0f; // Live ADC voltage

// Waterfall layout (SDR Style)
const int wf_w = 480;
const int wf_h = 100; // Better proportions
const int wf_x = 0;
const int wf_y = 90;
static uint16_t wf_buffer[480 * 100]; 

// FFT Scope Layout
const int sp_h = 54;
const int sp_y = 36;

// Convert 0.0-1.0 magnitude to an SDR-style heatmap color
static inline uint16_t get_heatmap_color(float val) {
    if (val < 0.0f) val = 0.0f;
    if (val > 1.0f) val = 1.0f;
    
    int r = 0, g = 0, b = 0;
    if (val < 0.2f) {
        b = val * 5.0f * 255;
    } else if (val < 0.4f) {
        b = 255;
        r = (val - 0.2f) * 5.0f * 255;
    } else if (val < 0.6f) {
        r = 255;
        b = 255 - (val - 0.4f) * 5.0f * 255;
    } else if (val < 0.8f) {
        r = 255;
        g = (val - 0.6f) * 5.0f * 255;
    } else {
        r = 255;
        g = 255;
        b = (val - 0.8f) * 5.0f * 255;
    }
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

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

    // Initialize custom display driver (Ping-Pong DMA)
    ili9341_init(); 
    ili9341_fill_screen(COLOR_BLACK);

    memset(wf_buffer, 0, sizeof(wf_buffer));

    // UI Overlays using LGFX Sprites
    LGFX_Sprite top_bar(&tft);
    top_bar.setColorDepth(16);
    top_bar.createSprite(480, 36);

    LGFX_Sprite text_area(&tft);
    text_area.setColorDepth(16);
    text_area.createSprite(480, 130);
    
    LGFX_Sprite spectrum(&tft);
    spectrum.setColorDepth(16);
    spectrum.createSprite(480, sp_h);

    int16_t tune_x = 240;
    const int shift = 17; // Roughly 170Hz on the screen
    
    const int bin_start = 31;  // ~300 Hz
    const int bin_end = 338;   // ~3300 Hz
    const float bin_per_pixel = (float)(bin_end - bin_start) / 480.0f;

    uint32_t last_touch = time_us_32();
    uint32_t last_ui_update = time_us_32();
    uint32_t frame_count = 0;
    float local_mag[FFT_SIZE / 2];

    while (true) {
        uint32_t now = time_us_32();

        // A. Poll Touch
        if (now - last_touch > 50000) {
            uint16_t tx, ty;
            if (tft.getTouch(&tx, &ty)) {
                if (ty >= sp_y && ty <= (wf_y + wf_h)) {
                    tune_x = tx;
                }
            }
            last_touch = now;
        }

        // B. Update UI Elements Periodically (2 FPS)
        if (now - last_ui_update > 500000) {
            uint32_t fps = frame_count * 2;
            frame_count = 0;
            last_ui_update = now;

            // Render Top Bar
            top_bar.fillSprite(tft.color565(26, 26, 26));
            top_bar.setTextColor(tft.color565(0, 255, 0));
            top_bar.setTextSize(2);
            top_bar.setCursor(10, 10);
            top_bar.print("2125.0 Hz");
            top_bar.setTextSize(1);
            top_bar.setTextColor(TFT_WHITE);
            top_bar.setCursor(130, 12);
            top_bar.printf("RTTY-45 | S:170 | ADC: %.2fV | FPS: %lu", shared_adc_v, fps);
            ili9488_push_colors(0, 0, 480, 36, (uint16_t*)top_bar.getBuffer());

            // Render Text Area
            text_area.fillSprite(TFT_BLACK);
            text_area.setTextColor(tft.color565(0, 255, 0));
            text_area.setTextSize(2);
            text_area.setCursor(5, 5);
            text_area.println("DECODER READY.");
            text_area.printf("\nLive ADC: %.2f V\n", shared_adc_v);
            if (shared_adc_v < 0.1f) text_area.setTextColor(TFT_RED), text_area.println("WARN: ADC grounded?");
            else if (shared_adc_v > 3.2f) text_area.setTextColor(TFT_RED), text_area.println("WARN: ADC clipped?");
            else text_area.setTextColor(TFT_GREEN), text_area.println("ADC nominal range.");
            ili9488_push_colors(0, 190, 480, 130, (uint16_t*)text_area.getBuffer());
        }

        // C. Render Loop (Syncs with FFT readiness, ~30 FPS)
        if (new_fft_ready) {
            frame_count++;
            memcpy(local_mag, (void*)shared_fft_mag, sizeof(local_mag));
            new_fft_ready = false;

            // 1. Scroll Waterfall
            memmove(&wf_buffer[wf_w], &wf_buffer[0], wf_w * (wf_h - 1) * sizeof(uint16_t));
            
            // 2. Generate Spectrum
            float min_db = 20.0f; // Noise floor
            float max_db = 55.0f; // Peak signal
            
            spectrum.fillSprite(tft.color565(5,5,5)); // Clear spectrum sprite

            for (int x = 0; x < 480; x++) {
                // Interpolate bins for smoothness
                float exact_bin = bin_start + x * bin_per_pixel;
                int b0 = (int)exact_bin;
                int b1 = b0 + 1;
                float frac = exact_bin - b0;
                float db = local_mag[b0] * (1.0f - frac) + local_mag[b1] * frac;
                
                // Waterfall Pixel
                float normalized = (db - min_db) / (max_db - min_db);
                wf_buffer[x] = get_heatmap_color(normalized);

                // Spectrum Pixel
                int h = (int)(normalized * sp_h);
                if (h > sp_h) h = sp_h;
                if (h > 0) {
                    spectrum.drawFastVLine(x, sp_h - h, h, tft.color565(0, 255, 255));
                }
            }

            // Draw Markers on Spectrum Sprite
            spectrum.drawFastVLine(tune_x, 0, sp_h, TFT_WHITE);
            spectrum.drawFastVLine(tune_x - shift, 0, sp_h, TFT_CYAN);
            spectrum.drawFastVLine(tune_x + shift, 0, sp_h, TFT_YELLOW);

            // Push Spectrum
            ili9488_push_colors(0, sp_y, 480, sp_h, (uint16_t*)spectrum.getBuffer());

            // Push Waterfall
            ili9488_push_waterfall(wf_x, wf_y, wf_w, wf_h, wf_buffer, tune_x, shift);
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
    const int SLIDE = 333; // ~30 FFTs per second
    
    // Initialize ADC directly on Core 0 for 100% control
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(0);

    while (true) {
        uint32_t start_time = time_us_32();
        
        // 1. Direct ADC Read (bulletproof, no DMA hangs)
        uint16_t raw_val = adc_read();
        
        // Calculate live ADC voltage for UI
        shared_adc_v = ((float)raw_val / 4095.0f) * 3.3f;

        // Normalize to -1.0 to 1.0 (12-bit ADC centered at 2048)
        float sample = ((float)raw_val - 2048.0f) / 2048.0f;
        
        real[samples_collected] = sample;
        imag[samples_collected] = 0.0f;
        
        samples_collected++;

        // 2. Process FFT when buffer is full
        if (samples_collected == FFT_SIZE) {
            fft.apply_window(real);
            fft.compute(real, imag);
            fft.calc_magnitude(real, imag, mag);

            // Safely update shared memory
            memcpy((void*)shared_fft_mag, mag, sizeof(mag));
            new_fft_ready = true;

            // Shift buffer down to create overlap for the next FFT frame
            memmove(real, &real[SLIDE], (FFT_SIZE - SLIDE) * sizeof(float));
            samples_collected = FFT_SIZE - SLIDE;
        }
        
        // 3. Precision timing: Wait until exactly 100us have passed since start (10kHz rate)
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
    printf("\n\nREAL AUDIO DSP WATERFALL STARTING...\n\n");

    multicore_launch_core1(core1_main);

    // Enter DSP loop directly (ADC is initialized inside the loop)
    core0_dsp_loop();
    return 0;
}