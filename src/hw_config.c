// TouchRTTY
// Copyright (C) 2026 Alexander Lavrinovich (Alex.Electron)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

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
