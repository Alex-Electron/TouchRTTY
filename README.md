# RTTYDecoder_v2 (Frankenstein Architecture)

## Project Overview
This project is an Autonomous Professional RTTY/CW/FT8 Digital Radio Terminal built on the Raspberry Pi Pico 2 (RP2350) and a 3.5-inch ILI9488 display (480x320).

## Current State: Phase 1 Complete
We have successfully built a "Frankenstein" architecture that combines the best of both worlds:
1. **Touch & UI Framework:** We use the `LovyanGFX` library (via SPI1) to manage the XPT2046 touch controller. It handles precise affine matrix calibration and touch coordinate mapping.
2. **Display Driver:** We bypassed LovyanGFX's display driver because it falls back to software bit-banging (~3 FPS) on the RP2350 when dealing with the strict 18-bit RGB666 requirement of the ILI9488 over 4-wire SPI.
3. **Custom 60MHz PIO DMA Engine:** We wrote a custom C++ driver (`src/display/ili9341_test.c` + `ili9488_spi.pio`) that drives the SPI0 bus at 60 MHz. It uses Ping-Pong DMA buffering to expand 16-bit colors to 24-bit on the fly.
4. **Result:** A rock-solid 29-30 FPS full-width waterfall (480x160 pixels) with zero marker flickering and near 0% CPU load during transmission.

## Hardware Configuration
* **MCU:** RP2350 (overclocked to 300 MHz)
* **Display Bus:** SPI0 (GPIO 18 SCK, 19 MOSI, 17 CS, 20 DC, 21 RST) - *MISO is disconnected.*
* **Touch Bus:** SPI1 (GPIO 10 SCK, 11 MOSI, 12 MISO, 15 CS)

## Next Steps
- Implement UI layout, fonts, and the actual DSP engine on Core 0.