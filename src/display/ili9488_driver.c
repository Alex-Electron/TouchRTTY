#include "ili9488_driver.h"
#include "ili9488_spi.pio.h"
#include "pico/time.h"
#include <stdlib.h>

static int dma_chan;
static dma_channel_config dma_conf;
static PIO pio_inst = pio0;
static uint pio_sm = 0;

static void write_command(uint8_t cmd) {
    gpio_put(PIN_DC, 0); gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI_PORT, &cmd, 1);
    gpio_put(PIN_CS, 1);
}

static void write_data(uint8_t data) {
    gpio_put(PIN_DC, 1); gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI_PORT, &data, 1);
    gpio_put(PIN_CS, 1);
}

static void ili9488_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    write_command(0x2A); 
    write_data(x0 >> 8); write_data(x0 & 0xFF);
    write_data(x1 >> 8); write_data(x1 & 0xFF);
    write_command(0x2B); 
    write_data(y0 >> 8); write_data(y0 & 0xFF);
    write_data(y1 >> 8); write_data(y1 & 0xFF);
    write_command(0x2C); 
}

void ili9488_init(void) {
    gpio_set_function(PIN_CS, GPIO_FUNC_SIO); gpio_set_dir(PIN_CS, GPIO_OUT); gpio_put(PIN_CS, 1);
    gpio_set_function(PIN_DC, GPIO_FUNC_SIO); gpio_set_dir(PIN_DC, GPIO_OUT); gpio_put(PIN_DC, 1);
    gpio_set_function(PIN_RST, GPIO_FUNC_SIO); gpio_set_dir(PIN_RST, GPIO_OUT); gpio_put(PIN_RST, 1);

    gpio_put(PIN_RST, 1); sleep_ms(10);
    gpio_put(PIN_RST, 0); sleep_ms(10);
    gpio_put(PIN_RST, 1); sleep_ms(120);

    spi_init(SPI_PORT, SPI_FREQ);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    write_command(0xE0); write_data(0x00); write_data(0x03); write_data(0x09); write_data(0x08); write_data(0x16); write_data(0x0A); write_data(0x3F); write_data(0x78); write_data(0x4C); write_data(0x09); write_data(0x0A); write_data(0x08); write_data(0x16); write_data(0x1A); write_data(0x0F);
    write_command(0xE1); write_data(0x00); write_data(0x16); write_data(0x19); write_data(0x03); write_data(0x0F); write_data(0x05); write_data(0x32); write_data(0x45); write_data(0x46); write_data(0x04); write_data(0x0E); write_data(0x0D); write_data(0x35); write_data(0x37); write_data(0x0F);
    write_command(0xC0); write_data(0x17); write_data(0x15);
    write_command(0xC1); write_data(0x41);
    write_command(0xC5); write_data(0x00); write_data(0x12); write_data(0x80);
    write_command(0x36); write_data(0x28); // Landscape, BGR Mode
    write_command(0x3A); write_data(0x66);
    write_command(0xB0); write_data(0x00);
    write_command(0xB1); write_data(0xA0);
    write_command(0xB4); write_data(0x02);
    write_command(0xB6); write_data(0x02); write_data(0x02); write_data(0x3B);
    write_command(0xB7); write_data(0xC6);
    write_command(0xF7); write_data(0xA9); write_data(0x51); write_data(0x2C); write_data(0x82);
    write_command(0x11); sleep_ms(120);
    write_command(0x21);
    write_command(0x29); sleep_ms(50);

    uint offset = pio_add_program(pio_inst, &ili9488_spi_program);
    ili9488_spi_program_init(pio_inst, pio_sm, offset, PIN_MOSI, PIN_SCK, PIO_FREQ);

    dma_chan = dma_claim_unused_channel(true);
    dma_conf = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dma_conf, DMA_SIZE_32);
    channel_config_set_dreq(&dma_conf, pio_get_dreq(pio_inst, pio_sm, true));

    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
}

// HARDCODED MODE 11 FROM BUILD 107
static inline uint32_t expand_color_dynamic(uint16_t color) {
    uint16_t c = (color >> 8) | (color << 8); 
    uint8_t r = (c >> 8) & 0xF8;
    uint8_t g = (c >> 3) & 0xFC;
    uint8_t b = (c << 3) & 0xF8;
    return ((uint32_t)b << 24) | ((uint32_t)g << 16) | ((uint32_t)r << 8);
}

static void switch_to_pio() {
    while (spi_is_busy(SPI_PORT));
    pio_sm_set_enabled(pio_inst, pio_sm, false);
    pio_sm_restart(pio_inst, pio_sm);
    pio_sm_set_enabled(pio_inst, pio_sm, true);
    gpio_set_function(PIN_SCK, GPIO_FUNC_PIO0);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_PIO0);
}

static void switch_to_spi() {
    while (!pio_sm_is_tx_fifo_empty(pio_inst, pio_sm));
    sleep_us(5);
    pio_sm_set_enabled(pio_inst, pio_sm, false);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
}

void ili9488_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (x > 479 || y > 319 || w == 0 || h == 0) return;
    if (x + w > 480) w = 480 - x;
    if (y + h > 320) h = 320 - y;

    uint32_t c32 = expand_color_dynamic(color);
    ili9488_set_window(x, y, x + w - 1, y + h - 1);
    
    static uint32_t line_buf[480];
    for(int i = 0; i < w; i++) line_buf[i] = c32;

    gpio_put(PIN_DC, 1);
    gpio_put(PIN_CS, 0);
    switch_to_pio();

    for(int row = 0; row < h; row++) {
        dma_channel_configure(dma_chan, &dma_conf, &pio_inst->txf[pio_sm], line_buf, w, true);
        dma_channel_wait_for_finish_blocking(dma_chan);
    }
    
    switch_to_spi();
    gpio_put(PIN_CS, 1);
}

void ili9488_push_colors(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t* colors) {
    if (!colors || x > 479 || y > 319 || w == 0 || h == 0) return;
    if (x + w > 480) w = 480 - x;
    if (y + h > 320) h = 320 - y;

    ili9488_set_window(x, y, x + w - 1, y + h - 1);
    
    static uint32_t c32_buf[480]; 

    gpio_put(PIN_DC, 1);
    gpio_put(PIN_CS, 0);
    switch_to_pio();

    for(int row = 0; row < h; row++) {
        for(int col = 0; col < w; col++) {
            c32_buf[col] = expand_color_dynamic(colors[row * w + col]);
        }
        dma_channel_configure(dma_chan, &dma_conf, &pio_inst->txf[pio_sm], c32_buf, w, true);
        dma_channel_wait_for_finish_blocking(dma_chan);
    }
    
    switch_to_spi();
    gpio_put(PIN_CS, 1);
}

void ili9488_push_waterfall(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t* colors, int16_t tune_x, int16_t shift) {
    if (!colors || x > 479 || y > 319 || w == 0 || h == 0) return;
    if (x + w > 480) w = 480 - x;
    if (y + h > 320) h = 320 - y;

    ili9488_set_window(x, y, x + w - 1, y + h - 1);
    
    static uint32_t buf[2][480]; 
    int current_buf = 0;

    uint32_t c_space = expand_color_dynamic(0x00FFU); // Red hex -> Blue visual
    uint32_t c_mark  = expand_color_dynamic(0x07E0U); // Green visual

    gpio_put(PIN_DC, 1);
    gpio_put(PIN_CS, 0);
    switch_to_pio();

    for(int row = 0; row < h; row++) {
        dma_channel_configure(dma_chan, &dma_conf, &pio_inst->txf[pio_sm], buf[current_buf], w, true);
        
        int next_row = row + 1;
        int next_buf = current_buf ^ 1;
        
        if (next_row < h) {
            int offset = next_row * w;
            for(int col = 0; col < w; col++) {
                // High-visibility dashed markers
                if ((row % 4 < 2) && (col == tune_x - shift)) buf[next_buf][col] = c_space;
                else if ((row % 4 < 2) && (col == tune_x + shift)) buf[next_buf][col] = c_mark;
                else buf[next_buf][col] = expand_color_dynamic(colors[offset + col]);
            }
        }
        
        dma_channel_wait_for_finish_blocking(dma_chan);
        current_buf = next_buf;
    }
    
    switch_to_spi();
    gpio_put(PIN_CS, 1);
}

void ili9488_fill_screen(uint16_t color) {
    ili9488_draw_rect(0, 0, 480, 320, color);
}
void ili9488_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color) {
    ili9488_draw_rect(x, y, w, 1, color);
}
void ili9488_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color) {
    ili9488_draw_rect(x, y, 1, h, color);
}
void ili9488_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    ili9488_draw_rect(x, y, 1, 1, color);
}
void ili9488_draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1; 
    int err = dx + dy, e2; 
    for(;;) {
        ili9488_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
void ili9488_draw_circle(uint16_t x0, uint16_t y0, uint16_t r, uint16_t color) {
    int f = 1 - r;
    int ddF_x = 1, ddF_y = -2 * r;
    int x = 0, y = r;
    ili9488_draw_pixel(x0, y0 + r, color);
    ili9488_draw_pixel(x0, y0 - r, color);
    ili9488_draw_pixel(x0 + r, y0, color);
    ili9488_draw_pixel(x0 - r, y0, color);
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x;
        ili9488_draw_pixel(x0 + x, y0 + y, color);
        ili9488_draw_pixel(x0 - x, y0 + y, color);
        ili9488_draw_pixel(x0 + x, y0 - y, color);
        ili9488_draw_pixel(x0 - x, y0 - y, color);
        ili9488_draw_pixel(x0 + y, y0 + x, color);
        ili9488_draw_pixel(x0 - y, y0 + x, color);
        ili9488_draw_pixel(x0 + y, y0 - x, color);
        ili9488_draw_pixel(x0 - y, y0 - x, color);
    }
}
