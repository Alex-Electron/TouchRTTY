#ifndef ILI9341_TEST_H
#define ILI9341_TEST_H

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

// Pin Definitions
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19
#define PIN_DC   20
#define PIN_RST  21

#define SPI_PORT spi0

// Colors (RGB565)
#define COLOR_BLACK   0x0000
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_WHITE   0xFFFF

#ifdef __cplusplus
extern "C" {
#endif

void ili9341_init(void);
void ili9341_fill_screen(uint16_t color);
void ili9488_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void ili9488_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color);
void ili9488_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color);
void ili9488_draw_palette_test(void);
void ili9488_set_inversion(bool on);
void ili9488_set_madctl(uint8_t val);
void ili9488_set_packing_mode(int mode);
void ili9488_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void ili9488_send_row_dma(const uint8_t *data, size_t len);
void ili9488_set_pio_freq(float freq);

#ifdef __cplusplus
}
#endif

#endif
