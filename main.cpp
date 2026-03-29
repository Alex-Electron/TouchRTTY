#include <stdio.h>
#include <math.h>
#include <algorithm>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "LGFX_Config.hpp"

LGFX_RP2350 tft;

// Standard Benchmark Functions
uint32_t testFillScreen() {
    uint32_t start = time_us_32();
    tft.fillScreen(TFT_WHITE);
    tft.fillScreen(TFT_RED);
    tft.fillScreen(TFT_GREEN);
    tft.fillScreen(TFT_BLUE);
    tft.fillScreen(TFT_BLACK);
    return (time_us_32() - start) / 5;
}

uint32_t testLines() {
    uint32_t start = time_us_32();
    for (int i = 0; i < 500; i++) {
        tft.drawLine(rand() % 480, rand() % 320, rand() % 480, rand() % 320, rand() % 0xFFFF);
    }
    return time_us_32() - start;
}

uint32_t testFilledRects() {
    uint32_t start = time_us_32();
    for (int i = 0; i < 200; i++) {
        tft.fillRect(rand() % 400, rand() % 240, rand() % 80, rand() % 80, rand() % 0xFFFF);
    }
    return time_us_32() - start;
}

uint32_t testCircles() {
    uint32_t start = time_us_32();
    for (int i = 0; i < 200; i++) {
        tft.drawCircle(rand() % 480, rand() % 320, rand() % 40, rand() % 0xFFFF);
    }
    return time_us_32() - start;
}

void run_benchmark() {
    printf("\n--- Benchmark Starting ---\n");
    
    // Check real clock frequency
    uint32_t sys_hz = clock_get_hz(clk_sys);
    printf("Real System Clock: %lu Hz\n", sys_hz);

    uint32_t t_fill = testFillScreen();
    uint32_t t_lines = testLines();
    uint32_t t_rects = testFilledRects();
    uint32_t t_circles = testCircles();

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(3);
    tft.setCursor(20, 40);
    
    tft.println("LGFX RP2350 PRO");
    tft.println("================");
    tft.setTextSize(2);
    tft.printf("Clock: %lu MHz\n", sys_hz / 1000000);
    tft.println("");
    
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.printf("Fill Screen:  %7lu us\n", t_fill);
    tft.printf("500 Lines:    %7lu us\n", t_lines);
    tft.printf("200 F-Rects:  %7lu us\n", t_rects);
    tft.printf("200 Circles:  %7lu us\n", t_circles);

    tft.println("");
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.println("Test Complete.");
    tft.println("Wait 15s...");

    printf("FillScreen: %lu us\n", t_fill);
    printf("500 Lines:  %lu us\n", t_lines);
    printf("200 F.Rects: %lu us\n", t_rects);
    printf("--- Benchmark Done ---\n");

    sleep_ms(15000);
}

void core1_main() {
    tft.init();
    tft.setRotation(1);
    while (true) {
        run_benchmark();
    }
}

int main() {
    // Attempt overclocking
    vreg_set_voltage(VREG_VOLTAGE_1_30); sleep_ms(10);
    bool ok = set_sys_clock_khz(300000, true);
    
    stdio_init_all();
    sleep_ms(2000);
    printf("\n\nLGFX SPEED TEST STARTING (Clock OK: %d)\n", ok);

    multicore_launch_core1(core1_main);

    while (true) { tight_loop_contents(); }
    return 0;
}