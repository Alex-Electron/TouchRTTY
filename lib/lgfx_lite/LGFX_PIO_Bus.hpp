#ifndef LGFX_PIO_BUS_HPP
#define LGFX_PIO_BUS_HPP

#include <LovyanGFX.hpp>
#include "hardware/pio.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "tft_io.pio.h"

namespace lgfx {
inline namespace v1 {

class Bus_PIO : public IBus {
public:
    struct config_t {
        uint8_t pio_host = 0;
        uint32_t freq_write = 60000000;
        int16_t pin_sclk = 18;
        int16_t pin_mosi = 19;
        int16_t pin_dc = 20;
    };

    const config_t& config(void) const { return _cfg; }
    void config(const config_t& config) { _cfg = config; }
    bus_type_t busType(void) const override { return bus_type_t::bus_spi; }

    bool init(void) override {
        lgfxPinMode(_cfg.pin_dc, pin_mode_t::output);
        _mask_dc = 1ul << _cfg.pin_dc;

        // Init Hardware SPI0 for 8-bit commands (Reliable Initialization)
        spi_init(spi0, 24000000); 
        spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST); // CRITICAL: Explicit SPI format

        // Init PIO for High-Speed 24-bit Video Data (60MHz)
        _pio = (_cfg.pio_host == 0) ? pio0 : pio1;
        _sm = pio_claim_unused_sm(_pio, true);
        uint offset = pio_add_program(_pio, &tft_io_program);
        
        pio_sm_config c = tft_io_program_get_default_config(offset);
        sm_config_set_out_pins(&c, _cfg.pin_mosi, 1);
        sm_config_set_sideset_pins(&c, _cfg.pin_sclk);
        sm_config_set_out_shift(&c, false, true, 24); // Autopull 24-bits
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        gpio_set_outover(_cfg.pin_sclk, GPIO_OVERRIDE_LOW);
        sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (_cfg.freq_write * 4));
        pio_sm_init(_pio, _sm, offset, &c);

        _dma_ch = dma_claim_unused_channel(true);
        _dma_cfg = dma_channel_get_default_config(_dma_ch);
        channel_config_set_transfer_data_size(&_dma_cfg, DMA_SIZE_32);
        channel_config_set_dreq(&_dma_cfg, pio_get_dreq(_pio, _sm, true));

        _current_mode = -1;
        _switch_spi();
        return true;
    }

    void release(void) override {
        dma_channel_unclaim(_dma_ch);
        pio_sm_unclaim(_pio, _sm);
        spi_deinit(spi0);
    }

    void beginTransaction(void) override {}
    void endTransaction(void) override { wait(); } // CRITICAL: Prevent CS from going high before shift register is empty
    
    void wait(void) override { 
        if (_current_mode == 1) {
            while (!pio_sm_is_tx_fifo_empty(_pio, _sm)); 
            sleep_us(2); // Wait for the PIO output shift register to finish
        } else {
            while (spi_is_busy(spi0)); // Wait for hardware SPI shift register to empty
        }
    }
    
    bool busy(void) const override { 
        return (_current_mode == 1) ? !pio_sm_is_tx_fifo_empty(_pio, _sm) : spi_is_busy(spi0); 
    }

    bool writeCommand(uint32_t data, uint_fast8_t bit_length) override {
        wait(); _switch_spi(); gpio_clr_mask(_mask_dc);
        uint8_t d = data; spi_write_blocking(spi0, &d, 1); 
        return true;
    }

    void writeData(uint32_t data, uint_fast8_t bit_length) override {
        wait(); _switch_spi(); gpio_set_mask(_mask_dc);
        if (bit_length == 16) {
            uint8_t d[2] = { (uint8_t)(data >> 8), (uint8_t)data };
            spi_write_blocking(spi0, d, 2);
        } else {
            uint8_t d = data; 
            spi_write_blocking(spi0, &d, 1);
        }
    }

    void writeDataRepeat(uint32_t data, uint_fast8_t bit_length, uint32_t count) override {
        wait(); gpio_set_mask(_mask_dc);
        if (bit_length == 16) {
            // LGFX sends 16-bit color blocks. We intercept and expand to 24-bit PIO!
            _switch_pio();
            uint32_t c32 = _expand(data);
            
            const uint32_t CHUNK = 1024;
            static uint32_t fill_buf[CHUNK];
            for(uint32_t i=0; i<CHUNK; i++) fill_buf[i] = c32;

            while(count > 0) {
                uint32_t len = (count > CHUNK) ? CHUNK : count;
                dma_channel_configure(_dma_ch, &_dma_cfg, &_pio->txf[_sm], fill_buf, len, true);
                dma_channel_wait_for_finish_blocking(_dma_ch);
                count -= len;
            }
        } else {
            _switch_spi(); 
            uint8_t d = data;
            for(uint32_t i=0; i<count; i++) spi_write_blocking(spi0, &d, 1);
        }
    }

    void writePixels(pixelcopy_t* param, uint32_t length) override {
        wait(); _switch_pio(); gpio_set_mask(_mask_dc);
        
        const uint32_t CHUNK = 512;
        uint16_t src_buf[CHUNK];
        static uint32_t dma_buf[CHUNK];

        while (length > 0) {
            uint32_t len = (length > CHUNK) ? CHUNK : length;
            param->fp_copy(src_buf, 0, len, param); // Extract 16-bit colors from LGFX
            
            for(uint32_t i=0; i<len; i++) {
                dma_buf[i] = _expand(src_buf[i]);  // Blast 24-bit to PIO
            }

            dma_channel_configure(_dma_ch, &_dma_cfg, &_pio->txf[_sm], dma_buf, len, true);
            dma_channel_wait_for_finish_blocking(_dma_ch);
            length -= len;
        }
    }

    void writeBytes(const uint8_t* data, uint32_t length, bool dc, bool use_dma) override {
        wait(); _switch_spi();
        if (dc) gpio_set_mask(_mask_dc); else gpio_clr_mask(_mask_dc);
        spi_write_blocking(spi0, data, length);
    }

    // Required Interface Stubs
    void initDMA(void) override {}
    void flush(void) override { wait(); } // CRITICAL: Wait before returning
    void addDMAQueue(const uint8_t* data, uint32_t length) override { writeBytes(data, length, true, false); }
    void execDMAQueue(void) override {}
    uint8_t* getDMABuffer(uint32_t length) override { return nullptr; }
    void beginRead(void) override {}
    void endRead(void) override {}
    uint32_t readData(uint_fast8_t bit_length) override { return 0; }
    bool readBytes(uint8_t* dst, uint32_t length, bool use_dma) override { return false; }
    void readPixels(void* dst, pixelcopy_t* param, uint32_t length) override {}

private:
    config_t _cfg;
    PIO _pio;
    uint _sm;
    uint32_t _mask_dc;
    int _current_mode; 
    int _dma_ch;
    dma_channel_config _dma_cfg;

    void _switch_spi() {
        if (_current_mode != 0) {
            if (_current_mode == 1) { 
                while (!pio_sm_is_tx_fifo_empty(_pio, _sm)); 
                sleep_us(2); // CRITICAL: Wait for last bits to leave the OSR before switching mux
                pio_sm_set_enabled(_pio, _sm, false); 
            }
            gpio_set_function(_cfg.pin_mosi, GPIO_FUNC_SPI);
            gpio_set_function(_cfg.pin_sclk, GPIO_FUNC_SPI);
            _current_mode = 0;
        }
    }

    void _switch_pio() {
        if (_current_mode != 1) {
            while (spi_is_busy(spi0));
            gpio_set_function(_cfg.pin_mosi, GPIO_FUNC_PIO0);
            gpio_set_function(_cfg.pin_sclk, GPIO_FUNC_PIO0);
            pio_sm_restart(_pio, _sm);
            pio_sm_set_enabled(_pio, _sm, true);
            _current_mode = 1;
        }
    }

    inline uint32_t _expand(uint16_t color565) {
        // Swap bytes because LovyanGFX fp_copy outputs natively, but ILI9488 might need swapped order
        uint8_t r = (color565 >> 11) & 0x1F;
        uint8_t g = (color565 >> 5) & 0x3F;
        uint8_t b = color565 & 0x1F;
        uint8_t r6 = (r << 1) | (r >> 4);
        uint8_t b6 = (b << 1) | (b >> 4);
        return ((uint32_t)r6 << 26) | ((uint32_t)g << 20) | ((uint32_t)b6 << 14);
    }
};

}
}
#endif