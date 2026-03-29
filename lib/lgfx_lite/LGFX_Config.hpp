#ifndef LGFX_PICO_CONFIG_HPP
#define LGFX_PICO_CONFIG_HPP

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// This is a custom configuration class for LovyanGFX on RP2350
// Specifically tailored for ILI9488 480x320 with 18-bit PIO DMA.

class LGFX_RP2350 : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488     _panel_instance;
    lgfx::Bus_SPI           _bus_instance;
    lgfx::Touch_XPT2046     _touch_instance;

public:
    LGFX_RP2350(void) {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host    = 0;                // SPI0
            cfg.spi_mode    = 0;
            cfg.freq_write  = 60000000;         // 60 MHz PIO
            cfg.freq_read   = 16000000;
            cfg.pin_sclk    = 18;
            cfg.pin_mosi    = 19;
            cfg.pin_miso    = -1;               // MISO not used for display
            cfg.pin_dc      = 20;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = 17;
            cfg.pin_rst          = 21;
            cfg.pin_busy         = -1;
            cfg.memory_width     = 320;         // ILI9488 native RAM width
            cfg.memory_height    = 480;         // ILI9488 native RAM height
            cfg.panel_width      = 320;         // Physical width
            cfg.panel_height     = 480;         // Physical height
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;           // Keep default. We will rotate in main.cpp
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = true;        
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel_instance.config(cfg);
        }

        {
            auto cfg = _touch_instance.config();
            cfg.x_min      = 300;
            cfg.x_max      = 3900;
            cfg.y_min      = 300;
            cfg.y_max      = 3900;
            cfg.pin_int    = 14;
            cfg.bus_shared = false;
            cfg.spi_host   = 1;                 // Touch is on SPI1
            cfg.freq       = 2500000;
            cfg.pin_sclk   = 10;
            cfg.pin_mosi   = 11;
            cfg.pin_miso   = 12;
            cfg.pin_cs     = 15;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
        setPanel(&_panel_instance);
    }
};

#endif