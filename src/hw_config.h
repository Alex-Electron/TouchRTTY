
#pragma once

#include <stddef.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "ff.h"
#include "sd_card.h"
#include "spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Functions provided by hw_config.c */
size_t sd_get_num();
sd_card_t *sd_get_by_num(size_t num);
size_t spi_get_num();
spi_t *spi_get_by_num(size_t num);

#ifdef __cplusplus
}
#endif
