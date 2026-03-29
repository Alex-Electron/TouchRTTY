#include "ili9341_test.h"
#include "pico/time.h"

// Helper macros
#define LCD_CS_LOW()  gpio_put(PIN_CS, 0)
#define LCD_CS_HIGH() gpio_put(PIN_CS, 1)
#define LCD_DC_CMD()  gpio_put(PIN_DC, 0)
#define LCD_DC_DAT()  gpio_put(PIN_DC, 1)

static int dma_chan;
static dma_channel_config dma_conf;

static void write_command(uint8_t cmd) {
    LCD_DC_CMD();
    LCD_CS_LOW();
    spi_write_blocking(SPI_PORT, &cmd, 1);
    LCD_CS_HIGH();
}

static void write_data(uint8_t data) {
    LCD_DC_DAT();
    LCD_CS_LOW();
    spi_write_blocking(SPI_PORT, &data, 1);
    LCD_CS_HIGH();
}

void ili9488_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    write_command(0x2A); 
    write_data(x0 >> 8); write_data(x0 & 0xFF);
    write_data(x1 >> 8); write_data(x1 & 0xFF);
    
    write_command(0x2B); 
    write_data(y0 >> 8); write_data(y0 & 0xFF);
    write_data(y1 >> 8); write_data(y1 & 0xFF);

    write_command(0x2C); 
}

void ili9488_set_inversion(bool on) {
    write_command(on ? 0x21 : 0x20);
}

void ili9488_set_madctl(uint8_t val) {
    write_command(0x36);
    write_data(val);
}

void ili9341_init(void) {
    gpio_put(PIN_RST, 1); sleep_ms(100);
    gpio_put(PIN_RST, 0); sleep_ms(100);
    gpio_put(PIN_RST, 1); sleep_ms(300);

    spi_init(SPI_PORT, SPI_FREQ);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // ILI9488 Standard Init
    write_command(0xE0); write_data(0x00); write_data(0x03); write_data(0x09); write_data(0x08); write_data(0x16); write_data(0x0A); write_data(0x3F); write_data(0x78); write_data(0x4C); write_data(0x09); write_data(0x0A); write_data(0x08); write_data(0x16); write_data(0x1A); write_data(0x0F);
    write_command(0xE1); write_data(0x00); write_data(0x16); write_data(0x19); write_data(0x03); write_data(0x0F); write_data(0x05); write_data(0x32); write_data(0x45); write_data(0x46); write_data(0x04); write_data(0x0E); write_data(0x0D); write_data(0x35); write_data(0x37); write_data(0x0F);
    write_command(0xC0); write_data(0x17); write_data(0x15);
    write_command(0xC1); write_data(0x41);
    write_command(0xC5); write_data(0x00); write_data(0x12); write_data(0x80);
    
    // MADCTL: Rotate 180, BGR=0
    write_command(0x36); write_data(0xE0); 
    
    write_command(0x3A); write_data(0x66); 
    write_command(0xB0); write_data(0x00);
    write_command(0xB1); write_data(0xA0);
    write_command(0xB4); write_data(0x02);
    write_command(0xB6); write_data(0x02); write_data(0x02); write_data(0x3B);
    write_command(0xB7); write_data(0xC6);
    write_command(0xF7); write_data(0xA9); write_data(0x51); write_data(0x2C); write_data(0x82);

    write_command(0x11); sleep_ms(150);
    
    // Set Inversion ON to fix the "White is Black" issue
    write_command(0x21); 

    write_command(0x29); sleep_ms(50);
}

void ili9488_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (x > 479 || y > 319 || w == 0 || h == 0) return;
    if (x + w > 480) w = 480 - x;
    if (y + h > 320) h = 320 - y;

    uint8_t r = (color >> 8) & 0xF8;
    uint8_t g = (color >> 3) & 0xFC;
    uint8_t b = (color << 3) & 0xF8;

    ili9488_set_window(x, y, x + w - 1, y + h - 1);
    
    static uint8_t line_buf[480 * 3];
    int idx = 0;
    for(int i = 0; i < w; i++) {
        line_buf[idx++] = r;
        line_buf[idx++] = g;
        line_buf[idx++] = b;
    }

    LCD_DC_DAT();
    LCD_CS_LOW();
    for(int row = 0; row < h; row++) {
        spi_write_blocking(SPI_PORT, line_buf, w * 3);
    }
    LCD_CS_HIGH();
}

void ili9488_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color) {
    ili9488_draw_rect(x, y, w, 1, color);
}

void ili9488_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color) {
    ili9488_draw_rect(x, y, 1, h, color);
}

void ili9341_fill_screen(uint16_t color) {
    ili9488_draw_rect(0, 0, 480, 320, color);
}

void ili9488_draw_palette_test(void) {
    ili9341_fill_screen(COLOR_BLACK);
    uint16_t h = 40;
    
    // 8 standard color bars
    ili9488_draw_rect(0, 0 * h, 480, h, COLOR_WHITE);
    ili9488_draw_rect(0, 1 * h, 480, h, COLOR_CYAN);
    ili9488_draw_rect(0, 2 * h, 480, h, COLOR_YELLOW);
    ili9488_draw_rect(0, 3 * h, 480, h, COLOR_GREEN);
    ili9488_draw_rect(0, 4 * h, 480, h, COLOR_MAGENTA);
    ili9488_draw_rect(0, 5 * h, 480, h, COLOR_RED);
    ili9488_draw_rect(0, 6 * h, 480, h, COLOR_BLUE);
    ili9488_draw_rect(0, 7 * h, 480, h, COLOR_BLACK);

    // Fat Professional Arrows (Clean Rectangles)
    uint16_t ax = 40, ay = 40, a_len = 100, a_thick = 8;
    
    // X Axis (Arrow Right)
    ili9488_draw_rect(ax, ay - (a_thick/2), a_len, a_thick, COLOR_WHITE); // Shaft
    for(int i=0; i<16; i++) {
        // Draw head cleanly: increasing X, decreasing height
        ili9488_draw_rect(ax + a_len + i, ay - 16 + i, 1, 32 - (i*2), COLOR_WHITE);
    }
    
    // Y Axis (Arrow Down)
    ili9488_draw_rect(ax - (a_thick/2), ay, a_thick, a_len, COLOR_WHITE); // Shaft
    for(int i=0; i<16; i++) {
        // Draw head cleanly: increasing Y, decreasing width
        ili9488_draw_rect(ax - 16 + i, ay + a_len + i, 32 - (i*2), 1, COLOR_WHITE);
    }
}