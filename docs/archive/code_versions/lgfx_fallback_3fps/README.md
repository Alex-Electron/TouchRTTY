# LovyanGFX Integration Attempt (Fallback Mode)

## Context
This directory contains the code state where we attempted to integrate the `LovyanGFX` library to drive the ILI9488 480x320 display on the RP2350 MCU.

## Findings & Results
- **Clock Speed:** Verified via `clock_get_hz(clk_sys)` that the RP2350 successfully overclocks to 300 MHz.
- **Benchmark Performance:** The standard LGFX benchmark returned **~364,000 us** for `tft.fillScreen()`.
- **Calculated FPS:** ~2.7 Frames Per Second.

## Conclusion
The `LovyanGFX` library (current GitHub master branch) **does not successfully engage hardware PIO/DMA acceleration for the 18-bit RGB666 ILI9488 panel on the new RP2350 architecture.** 

Instead of failing, the library silently falls back to a software bit-banging mode (manually toggling GPIOs to send 3 bytes per pixel). This consumes 100% of the 300MHz core's time just to push pixels, resulting in an unacceptable ~3 FPS refresh rate, rendering smooth waterfall displays impossible.

## Next Steps Considered
Due to the strict mandate for minimal CPU load and high performance, relying on a library that defaults to software SPI is not viable. The recommended path forward is to construct a custom, lightweight PIO + DMA driver specifically tailored for the RP2350's hardware constraints and the ILI9488's 18-bit color requirement.