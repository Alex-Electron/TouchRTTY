#ifndef LGFX_PICO_CONFIG_HPP
#define LGFX_PICO_CONFIG_HPP

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

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
            cfg.freq_write  = 40000000;         // 40 MHz
            cfg.freq_read   = 16000000;
            cfg.pin_sclk    = 18;
            cfg.pin_mosi    = 19;
            cfg.pin_miso    = 16;               // Restored for stability
            cfg.pin_dc      = 20;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = 17;
            cfg.pin_rst          = 21;
            cfg.pin_busy         = -1;
            cfg.memory_width     = 320;
            cfg.memory_height    = 480;
            cfg.panel_width      = 320;
            cfg.panel_height     = 480;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = true;        
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;       // STANDARD ILI9488 
            cfg.bus_shared       = false;
            _panel_instance.config(cfg);
        }

        {
            auto cfg = _touch_instance.config();
            cfg.x_min      = 4000;
            cfg.x_max      = 100;
            cfg.y_min      = 100;
            cfg.y_max      = 4000;
            cfg.pin_int    = -1; // disable int
            cfg.bus_shared = false;
            cfg.spi_host   = 1;                 
            cfg.freq       = 2000000;
            cfg.pin_sclk   = 10;
            cfg.pin_mosi   = 11;
            cfg.pin_miso   = 12;
            cfg.pin_cs     = 22; // CORRECT TOUCH CS
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
        setPanel(&_panel_instance);
    }
};

#endif