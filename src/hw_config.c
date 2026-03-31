
#include "hw_config.h"

/* SPI Configuration */
static spi_t spis[] = {
    {
        .hw_inst = spi1,  // Use SPI1
        .miso_gpio = 12,
        .mosi_gpio = 11,
        .sck_gpio = 10,
        .baud_rate = 12 * 1000 * 1000, // 12 MHz for SD Card
    }
};

/* SPI SD Card Configuration */
static sd_card_t sd_cards[] = {
    {
        .pcName = "0:",           // Logical drive name
        .type = SD_IF_SPI,        // Interface type: SPI
        .spi_if.spi = &spis[0],   // Point to SPI1 configuration
        .spi_if.ss_gpio = 13,     // Chip Select: GPIO 13
        .use_card_detect = false, // No dedicated CD pin
    }
};

/* Provide these functions to the library */
size_t sd_get_num() { return sizeof(sd_cards) / sizeof(sd_cards[0]); }
sd_card_t *sd_get_by_num(size_t num) {
    if (num < sd_get_num()) {
        return &sd_cards[num];
    } else {
        return NULL;
    }
}
size_t spi_get_num() { return sizeof(spis) / sizeof(spis[0]); }
spi_t *spi_get_by_num(size_t num) {
    if (num < spi_get_num()) {
        return &spis[num];
    } else {
        return NULL;
    }
}
