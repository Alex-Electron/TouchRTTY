#include "ili9341_test.h"
#include "pico/time.h"
#include "hardware/spi.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "ili9488_spi.pio.h"

// Helper macros
#define LCD_CS_LOW()  gpio_put(PIN_CS, 0)
#define LCD_CS_HIGH() gpio_put(PIN_CS, 1)
#define LCD_DC_CMD()  gpio_put(PIN_DC, 0)
#define LCD_DC_DAT()  gpio_put(PIN_DC, 1)

static int dma_chan;
static dma_channel_config dma_conf;
static PIO pio_inst = pio0;
static uint pio_sm = 0;

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
    write_command(0x2A); // Column addr set
    write_data(x0 >> 8); write_data(x0 & 0xFF);
    write_data(x1 >> 8); write_data(x1 & 0xFF);
    
    write_command(0x2B); // Row addr set
    write_data(y0 >> 8); write_data(y0 & 0xFF);
    write_data(y1 >> 8); write_data(y1 & 0xFF);

    write_command(0x2C); // Memory write
}

static float current_pio_freq = 60000000.0f; // 60MHz is stable, max is 90MHz
static uint pio_offset = 0;

void ili9488_set_pio_freq(float freq) {
    current_pio_freq = freq;
    pio_sm_set_enabled(pio_inst, pio_sm, false);
    ili9488_spi_program_init(pio_inst, pio_sm, pio_offset, PIN_MOSI, PIN_SCK, current_pio_freq);
}

void ili9341_init(void) {
    // 1. Hardware Reset
    gpio_put(PIN_RST, 1);
    sleep_ms(100);
    gpio_put(PIN_RST, 0);
    sleep_ms(100);
    gpio_put(PIN_RST, 1);
    sleep_ms(300);

    // 2. Initialization sequence (ILI9488) - Standard SPI at 62.5MHz for commands
    spi_init(SPI_PORT, 62500000);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    write_command(0xE0); // Positive Gamma
    write_data(0x00); write_data(0x03); write_data(0x09); write_data(0x08);
    write_data(0x16); write_data(0x0A); write_data(0x3F); write_data(0x78);
    write_data(0x4C); write_data(0x09); write_data(0x0A); write_data(0x08);
    write_data(0x16); write_data(0x1A); write_data(0x0F);

    write_command(0xE1); // Negative Gamma
    write_data(0x00); write_data(0x16); write_data(0x19); write_data(0x03);
    write_data(0x0F); write_data(0x05); write_data(0x32); write_data(0x45);
    write_data(0x46); write_data(0x04); write_data(0x0E); write_data(0x0D);
    write_data(0x35); write_data(0x37); write_data(0x0F);

    write_command(0xC0); write_data(0x17); write_data(0x15);
    write_command(0xC1); write_data(0x41);
    write_command(0xC5); write_data(0x00); write_data(0x12); write_data(0x80);

    write_command(0x36); // Memory Access Control
    write_data(0xE8);    // Rotate 180 degrees (MY=1, MX=1, MV=1, BGR=1/0)

    write_command(0x3A); // Pixel Format Set
    write_data(0x66);    // 18-bit pixel format (RGB666)

    write_command(0xB0); write_data(0x00);
    write_command(0xB1); write_data(0xA0);
    write_command(0xB4); write_data(0x02);
    write_command(0xB6); write_data(0x02); write_data(0x02); write_data(0x3B);
    write_command(0xB7); write_data(0xC6);
    write_command(0xF7); write_data(0xA9); write_data(0x51); write_data(0x2C); write_data(0x82);

    write_command(0x11); // Exit Sleep
    sleep_ms(250);
    write_command(0x29); // Display on
    sleep_ms(100);

    // 3. Setup PIO and DMA for bulk transfers
    pio_offset = pio_add_program(pio_inst, &ili9488_spi_program);
    ili9488_spi_program_init(pio_inst, pio_sm, pio_offset, PIN_MOSI, PIN_SCK, current_pio_freq);

    dma_chan = dma_claim_unused_channel(true);
    dma_conf = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dma_conf, DMA_SIZE_32);
    channel_config_set_dreq(&dma_conf, pio_get_dreq(pio_inst, pio_sm, true));
    dma_channel_configure(dma_chan, &dma_conf, &pio_inst->txf[pio_sm], NULL, 0, false);
}
static int current_packing_mode = 0;

void ili9488_set_inversion(bool on) {
    write_command(on ? 0x21 : 0x20);
}

void ili9488_set_madctl(uint8_t val) {
    write_command(0x36);
    write_data(val);
}

void ili9488_set_packing_mode(int mode) {
    current_packing_mode = mode;
}

void ili9488_draw_palette_test(void) {
    ili9341_fill_screen(COLOR_BLACK);
    
    // Background bars - pure test of the RGB mapping
    ili9488_draw_rect(0, 50, 160, 270, COLOR_RED);
    ili9488_draw_rect(160, 50, 160, 270, COLOR_GREEN);
    ili9488_draw_rect(320, 50, 160, 270, COLOR_BLUE);

    // Fat Arrow X (White)
    uint16_t x_off = 20, y_off = 20;
    ili9488_draw_rect(x_off, y_off, 100, 4, COLOR_WHITE); 
    for(int i=0; i<10; i++) ili9488_draw_rect(x_off + 100 - i, y_off - i + 2, 1, i*2, COLOR_WHITE);

    // Fat Arrow Y (White)
    ili9488_draw_rect(x_off, y_off, 4, 100, COLOR_WHITE); 
    for(int i=0; i<10; i++) ili9488_draw_rect(x_off - i + 2, y_off + 100 - i, i*2, 1, COLOR_WHITE);
}

void ili9488_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (x >= 480 || y >= 320) return;
    if (x + w > 480) w = 480 - x;
    if (y + h > 320) h = 320 - y;

    uint8_t r5 = (color >> 11) & 0x1F;
    uint8_t g6 = (color >> 5) & 0x3F;
    uint8_t b5 = color & 0x1F;

    // ILI9488 18-bit SPI format requires the 6 bits of color data to be 
    // left-aligned within the byte (MSB aligned). The bottom 2 bits are ignored.
    // R: 5 bits -> shift left 3. G: 6 bits -> shift left 2. B: 5 bits -> shift left 3.
    uint8_t r8 = r5 << 3; 
    uint8_t g8 = g6 << 2;
    uint8_t b8 = b5 << 3;

    // Apply color correction based on packing mode
    uint32_t packed_color;
    switch(current_packing_mode) {
        case 0: packed_color = ((uint32_t)r8) | ((uint32_t)g8 << 8) | ((uint32_t)b8 << 16); break; // R-G-B (Expected)
        case 1: packed_color = ((uint32_t)b8) | ((uint32_t)g8 << 8) | ((uint32_t)r8 << 16); break; // B-G-R
        case 2: packed_color = ((uint32_t)r8 << 8) | ((uint32_t)g8 << 16) | ((uint32_t)b8 << 24); break; // Shifted RGB
        case 3: packed_color = ((uint32_t)b8 << 8) | ((uint32_t)g8 << 16) | ((uint32_t)r8 << 24); break; // Shifted BGR
        default: packed_color = ((uint32_t)r8) | ((uint32_t)g8 << 8) | ((uint32_t)b8 << 16);
    }

    ili9488_set_window(x, y, x + w - 1, y + h - 1);
    
    static uint32_t rect_buf[480];
    for(int i = 0; i < w; i++) rect_buf[i] = packed_color;

    LCD_DC_DAT();
    LCD_CS_LOW();
    gpio_set_function(PIN_SCK, GPIO_FUNC_PIO0);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_PIO0);

    for(int row = 0; row < h; row++) {
        dma_channel_transfer_from_buffer_now(dma_chan, rect_buf, w);
        dma_channel_wait_for_finish_blocking(dma_chan);
    }
    
    while(!pio_sm_is_tx_fifo_empty(pio_inst, pio_sm)) {}
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    LCD_CS_HIGH();
}

void ili9341_fill_screen(uint16_t color) {
    ili9488_draw_rect(0, 0, 480, 320, color);
}