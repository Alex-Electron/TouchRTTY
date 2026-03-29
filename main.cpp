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

// --- BENCHMARK FUNCTIONS ON CUSTOM DRIVER ---

uint32_t testFillScreen() {
    uint32_t start = time_us_32();
    ili9341_fill_screen(COLOR_WHITE);
    ili9341_fill_screen(COLOR_RED);
    ili9341_fill_screen(COLOR_GREEN);
    ili9341_fill_screen(COLOR_BLUE);
    ili9341_fill_screen(COLOR_BLACK);
    return (time_us_32() - start) / 5;
}

// NOTE: Pixel-by-pixel drawing sets a 1x1 window over SPI for EVERY pixel.
// This is an intentional worst-case scenario for block-optimized drivers.
uint32_t testLines() {
    uint32_t start = time_us_32();
    for (int i = 0; i < 500; i++) {
        ili9488_draw_line(rand() % 480, rand() % 320, rand() % 480, rand() % 320, rand() % 0xFFFF);
    }
    return time_us_32() - start;
}

uint32_t testFilledRects() {
    uint32_t start = time_us_32();
    for (int i = 0; i < 200; i++) {
        ili9488_draw_rect(rand() % 400, rand() % 240, rand() % 80, rand() % 80, rand() % 0xFFFF);
    }
    return time_us_32() - start;
}

uint32_t testCircles() {
    uint32_t start = time_us_32();
    for (int i = 0; i < 200; i++) {
        ili9488_draw_circle(rand() % 480, rand() % 320, rand() % 40, rand() % 0xFFFF);
    }
    return time_us_32() - start;
}

void core1_main() {
    LGFX_RP2350 tft;
    tft.init();
    tft.setRotation(1); 
    
    // We keep LGFX around just to render nice text into a RAM sprite
    LGFX_Sprite text_sprite(&tft);
    text_sprite.setColorDepth(16);
    text_sprite.createSprite(480, 160);

    // Initialize our custom 60MHz PIO DMA driver
    printf("Switching to Custom High-Speed PIO Driver...\n");
    ili9341_init(); 
    
    while (true) {
        printf("\n--- Custom Driver Benchmark Starting ---\n");
        ili9341_fill_screen(COLOR_BLACK);
        
        uint32_t t_fill = testFillScreen();
        uint32_t t_lines = testLines();
        uint32_t t_rects = testFilledRects();
        uint32_t t_circles = testCircles();

        // Print results to screen using LGFX Sprite pushed via Custom DMA
        ili9341_fill_screen(COLOR_BLACK);
        
        text_sprite.fillSprite(TFT_BLACK);
        text_sprite.setTextColor(TFT_YELLOW);
        text_sprite.setTextSize(3);
        text_sprite.setCursor(20, 10);
        text_sprite.println("CUSTOM DRIVER BENCHMARK");
        text_sprite.println("=======================");
        
        text_sprite.setTextSize(2);
        text_sprite.setTextColor(TFT_WHITE);
        text_sprite.printf("Fill Screen: %7lu us\n", t_fill);
        text_sprite.printf("500 Lines:   %7lu us\n", t_lines);
        text_sprite.printf("200 F-Rects: %7lu us\n", t_rects);
        text_sprite.printf("200 Circles: %7lu us\n", t_circles);
        
        // Push the entire rendered text block to the middle of the screen via our ultra-fast DMA
        ili9488_push_colors(0, 80, 480, 160, (uint16_t*)text_sprite.getBuffer());

        printf("FillScreen: %lu us\n", t_fill);
        printf("500 Lines:  %lu us\n", t_lines);
        printf("200 F.Rects: %lu us\n", t_rects);
        printf("200 Circles: %lu us\n", t_circles);

        sleep_ms(15000);
    }
}

void core0_dsp_loop() {
    while (true) { tight_loop_contents(); }
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10);
    set_sys_clock_khz(300000, true);
    
    stdio_init_all();
    sleep_ms(2000);

    multicore_launch_core1(core1_main);

    // Dummy ADC init to satisfy dependencies
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