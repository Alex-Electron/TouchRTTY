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

// --- Hardware Configuration ---
#define ADC_PIN 26
#define SAMPLE_RATE 10000
#define RING_BUFFER_SAMPLES 2048
#define RING_BUFFER_BYTES (RING_BUFFER_SAMPLES * 2)
#define RING_ALIGN_BITS 12 

uint16_t adc_ring_buffer[RING_BUFFER_SAMPLES] __attribute__((aligned(RING_BUFFER_BYTES)));
int dma_adc_chan;

void process_rtty_sample(float sample) { } // Placeholder

void core1_main() {
    printf("Core 1: Starting LovyanGFX for Touch Calibration...\n");
    
    LGFX_RP2350 tft;
    tft.init();
    tft.setRotation(1); 
    
    // 1. LovyanGFX Calibration
    uint16_t calData[8];
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(10, 10);
    tft.setTextSize(2);
    tft.println("Touch arrows to calibrate");
    tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 15);
    tft.fillScreen(TFT_BLACK);

    // 2. TAKEOVER: Initialize our custom 60MHz PIO DMA driver
    printf("Switching to Custom High-Speed PIO Driver...\n");
    ili9341_init(); // Re-initializes SPI0, sets up PIO and DMA, overriding LGFX's SPI0 setup
    
    // Clear screen fully using custom driver
    ili9341_fill_screen(COLOR_BLACK);

    // Make waterfall full width
    const int wf_w = 480;
    const int wf_h = 160;
    const int wf_x = 0;
    const int wf_y = 60;
    static uint16_t wf_buffer[480 * 160]; 
    memset(wf_buffer, 0, sizeof(wf_buffer));

    // Create an off-screen sprite for the top status bar using LovyanGFX
    // This allows us to use LGFX's beautiful font engine, but push the pixels via our own DMA!
    LGFX_Sprite top_bar(&tft);
    top_bar.setColorDepth(16);
    top_bar.createSprite(480, 24);
    
    // Initial draw to screen so it's not black
    top_bar.fillSprite(tft.color565(20, 20, 40));
    top_bar.setTextColor(TFT_WHITE);
    top_bar.setTextSize(2);
    top_bar.setCursor(5, 4);
    top_bar.printf("FRANKENSTEIN UI LOADING...");
    ili9488_push_colors(0, 0, 480, 24, (uint16_t*)top_bar.getBuffer());

    int16_t tune_x = 240;
    const int shift = 30;
    float sig_x = 100.0f;
    float sig_drift = 1.5f;

    uint32_t last_frame_time = time_us_32();
    uint32_t last_touch = time_us_32();
    uint32_t last_fps_report = time_us_32();
    uint32_t frame_count = 0;
    uint32_t current_fps = 0;

    while (true) {
        uint32_t now = time_us_32();

        // A. Poll Touch via LovyanGFX (Uses SPI1, perfectly safe)
        if (now - last_touch > 50000) {
            uint16_t tx, ty;
            if (tft.getTouch(&tx, &ty)) {
                if (ty >= wf_y && ty <= (wf_y + wf_h)) {
                    tune_x = tx - wf_x;
                }
            }
            last_touch = now;
        }

        // B. Render Loop (Target 60 FPS -> ~16.6ms)
        if (now - last_frame_time > 16666) {
            last_frame_time = now;

            // 1. Scroll Waterfall
            memmove(&wf_buffer[wf_w], &wf_buffer[0], wf_w * (wf_h - 1) * sizeof(uint16_t));
            
            // 2. Generate new noise line
            sig_x += sig_drift;
            if (sig_x > (wf_w - 60) || sig_x < 20) sig_drift = -sig_drift;

            static uint32_t noise_seed = 0x12345678; // Xorshift state

            for (int i = 0; i < wf_w; i++) {
                // Fast Xorshift32 algorithm to eliminate LCG visual repeating patterns ("waves")
                noise_seed ^= noise_seed << 13;
                noise_seed ^= noise_seed >> 17;
                noise_seed ^= noise_seed << 5;

                uint8_t noise = noise_seed % 30;
                uint8_t r = noise/4, g = noise/2, b = 10 + noise;
                uint16_t color = (r << 11) | (g << 5) | b;

                if (abs(i - (int)sig_x) < 2) color = COLOR_CYAN;
                else if (abs(i - ((int)sig_x + shift*2)) < 2) color = COLOR_YELLOW;

                wf_buffer[i] = color;
            }

            // 3. Fast Ping-Pong DMA Transfer (Draws waterfall AND markers simultaneously)
            ili9488_push_waterfall(wf_x, wf_y, wf_w, wf_h, wf_buffer, tune_x, shift);

            // 4. FPS Logic & Status Bar Rendering
            frame_count++;
            if (now - last_fps_report > 1000000) {
                current_fps = frame_count;
                frame_count = 0;
                last_fps_report = now;

                // Render text into LGFX Sprite (in RAM, takes ~1ms)
                top_bar.fillSprite(tft.color565(20, 20, 40));
                top_bar.setTextColor(TFT_WHITE);
                top_bar.setCursor(5, 4);
                top_bar.printf("FPS: %3d | PING-PONG DMA 60MHz", current_fps);

                // Push the sprite buffer to the screen using our Custom PIO DMA!
                ili9488_push_colors(0, 0, 480, 24, (uint16_t*)top_bar.getBuffer());
            }
        }
        
        if (multicore_fifo_rvalid()) (void)multicore_fifo_pop_blocking();
        tight_loop_contents();
    }
}

void core0_dsp_loop() {
    uint32_t last_read_idx = 0;
    while (true) {
        uint32_t current_write_idx = (dma_hw->ch[dma_adc_chan].write_addr - (uint32_t)adc_ring_buffer) / 2;
        while (last_read_idx != current_write_idx) {
            last_read_idx = (last_read_idx + 1) % RING_BUFFER_SAMPLES;
        }
        tight_loop_contents();
    }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10);
    set_sys_clock_khz(300000, true);
    
    stdio_init_all();
    sleep_ms(2000);
    printf("\n\nFRANKENSTEIN WATERFALL STARTING...\n\n");

    multicore_launch_core1(core1_main);

    adc_init(); adc_gpio_init(ADC_PIN); adc_select_input(0);
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv(48000000.0f / SAMPLE_RATE);
    dma_adc_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_adc_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, RING_ALIGN_BITS);
    channel_config_set_dreq(&c, DREQ_ADC);
    dma_channel_configure(dma_adc_chan, &c, adc_ring_buffer, &adc_hw->result, 0xFFFFFFFF, true);
    adc_run(true);

    core0_dsp_loop();
    return 0;
}