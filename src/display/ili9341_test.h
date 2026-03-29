#ifndef ILI9341_TEST_H
#define ILI9341_TEST_H

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

// Pin Definitions
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19
#define PIN_DC   20
#define PIN_RST  21

#define SPI_PORT spi0
#define SPI_FREQ 24000000 
#define PIO_FREQ 60000000

// Colors (RGB565)
#define COLOR_BLACK   0x0000
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_WHITE   0xFFFF
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

#ifdef __cplusplus
extern "C" {
#endif

void ili9341_init(void);
void ili9488_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void ili9488_push_colors(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t* colors);
void ili9488_push_waterfall(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t* colors, int16_t tune_x, int16_t shift);

extern volatile int shared_color_mode;
void ili9488_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color);
void ili9488_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color);
void ili9488_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void ili9488_draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void ili9488_draw_circle(uint16_t x0, uint16_t y0, uint16_t r, uint16_t color);
void ili9341_fill_screen(uint16_t color);

#ifdef __cplusplus
}
#endif

#endif