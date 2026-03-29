#ifndef LGFX_PIO_BUS_HPP
#define LGFX_PIO_BUS_HPP

#include <LovyanGFX.hpp>
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "tft_io.pio.h" // We will reuse the PIO program from earlier

namespace lgfx {
inline namespace v1 {

class Bus_PIO : public IBus {
public:
    struct config_t {
        uint8_t pio_host = 0;
        uint32_t freq_write = 60000000;
        int16_t pin_sclk = -1;
        int16_t pin_mosi = -1;
        int16_t pin_dc = -1;
    };

    const config_t& config(void) const { return _cfg; }
    void config(const config_t& config) { _cfg = config; }
    bus_type_t busType(void) const override { return bus_type_t::bus_spi; }

    bool init(void) override {
        _pio = (_cfg.pio_host == 0) ? pio0 : pio1;
        _sm = pio_claim_unused_sm(_pio, true);
        uint offset = pio_add_program(_pio, &tft_io_program);
        
        tft_pio_init(_pio, _sm, offset, _cfg.pin_mosi, _cfg.pin_sclk, (float)clock_get_hz(clk_sys) / (_cfg.freq_write * 4));

        lgfxPinMode(_cfg.pin_dc, pin_mode_t::output);
        _mask_dc = 1ul << _cfg.pin_dc;

        _dma_ch = dma_claim_unused_channel(true);
        _dma_cfg = dma_channel_get_default_config(_dma_ch);
        channel_config_set_transfer_data_size(&_dma_cfg, DMA_SIZE_32);
        channel_config_set_dreq(&_dma_cfg, pio_get_dreq(_pio, _sm, true));
        return true;
    }

    void release(void) override {
        dma_channel_unclaim(_dma_ch);
        pio_sm_unclaim(_pio, _sm);
    }

    void beginTransaction(void) override {}
    void endTransaction(void) override {}
    void wait(void) override { while (!pio_sm_is_tx_fifo_empty(_pio, _sm)); }
    bool busy(void) const override { return !pio_sm_is_tx_fifo_empty(_pio, _sm); }

    bool writeCommand(uint32_t data, uint_fast8_t bit_length) override {
        wait();
        gpio_clr_mask(_mask_dc);
        _pio->txf[_sm] = data << (32 - bit_length);
        return true;
    }

    void writeData(uint32_t data, uint_fast8_t bit_length) override {
        wait();
        gpio_set_mask(_mask_dc);
        _pio->txf[_sm] = data << (32 - bit_length);
    }

    void writePixels(pixelcopy_t* param, uint32_t length) override {
        gpio_set_mask(_mask_dc);
        uint32_t buf[length];
        param->fp_copy(buf, 0, length, param);
        // This is a simplified software fallback for individual pixels, 
        // real DMA happens in writeBytes/pushImage
        for(uint32_t i=0; i<length; i++) {
            while(pio_sm_is_tx_fifo_full(_pio, _sm));
            _pio->txf[_sm] = buf[i]; 
        }
    }

    void writeBytes(const uint8_t* data, uint32_t length, bool dc, bool use_dma) override {
        if (dc) gpio_set_mask(_mask_dc); else gpio_clr_mask(_mask_dc);
        if (use_dma) {
            dma_channel_configure(_dma_ch, &_dma_cfg, &_pio->txf[_sm], data, length / 4, true);
            dma_channel_wait_for_finish_blocking(_dma_ch);
        } else {
            const uint32_t* p = (const uint32_t*)data;
            for(uint32_t i=0; i<length/4; i++) {
                while(pio_sm_is_tx_fifo_full(_pio, _sm));
                _pio->txf[_sm] = p[i];
            }
        }
    }

    // Unused for now
    void initDMA(void) override {}
    void flush(void) override {}
    void addDMAQueue(const uint8_t* data, uint32_t length) override { writeBytes(data, length, true, true); }
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
    int _dma_ch;
    dma_channel_config _dma_cfg;
};

}
}

#endif