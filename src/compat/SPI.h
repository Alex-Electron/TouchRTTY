#ifndef ARDUINO_SPI_H
#define ARDUINO_SPI_H

#include "hardware/spi.h"
#include "hardware/gpio.h"

#ifndef MSBFIRST
#define MSBFIRST 1
#endif
#ifndef SPI_MODE0
#define SPI_MODE0 0
#endif

class SPISettings {
public:
    SPISettings(uint32_t freq, uint8_t bitOrder, uint8_t mode) : _freq(freq) {}
    uint32_t _freq;
};

class SPIClass {
private:
    spi_inst_t* _spi;
    int _miso, _mosi, _sclk;
public:
    SPIClass(spi_inst_t* port, int miso, int cs, int sclk, int mosi) : _spi(port), _miso(miso), _mosi(mosi), _sclk(sclk) {}
    
    void begin() {
        spi_init(_spi, 1000000);
        gpio_set_function(_mosi, GPIO_FUNC_SPI);
        gpio_set_function(_sclk, GPIO_FUNC_SPI);
        if (_miso >= 0) gpio_set_function(_miso, GPIO_FUNC_SPI);
    }
    
    void beginTransaction(SPISettings settings) {
        spi_set_baudrate(_spi, settings._freq);
    }
    
    void endTransaction() {}
    
    uint8_t transfer(uint8_t data) {
        uint8_t rx;
        spi_write_read_blocking(_spi, &data, &rx, 1);
        return rx;
    }

    uint16_t transfer16(uint16_t data) {
        uint16_t rx;
        spi_write16_read16_blocking(_spi, &data, &rx, 1);
        return rx;
    }

    void setFrequency(uint32_t freq) {
        spi_set_baudrate(_spi, freq);
    }
};

// Aliases for TFT_eSPI
#define SPIClassRP2040 SPIClass

#endif
