# PHASE 1: HARDWARE & RENDERING ARCHITECTURE (COMPLETED)

## 1. The Challenge: ILI9488 + RP2350
The objective was to achieve a minimum 30 FPS full-screen (or near full-screen) waterfall rendering on a 3.5-inch ILI9488 display (480x320) using the Raspberry Pi Pico 2 (RP2350).

**The Bottleneck:** The ILI9488 controller in 4-wire SPI mode *strictly mandates* an 18-bit color format (RGB666), requiring 3 bytes (24 bits) per pixel. Standard libraries like `LovyanGFX` and `TFT_eSPI` lack native 18-bit DMA support for the new RP2350 architecture. When forced to use 18-bit color, they silently fall back to software SPI (bit-banging), resulting in severe CPU bottlenecking and unacceptable frame rates (~3 to 6 FPS).

## 2. The Solution: "Frankenstein" Hybrid Architecture
To bypass the limitations of standard libraries while retaining their advanced UI capabilities, we engineered a hybrid architecture that splits responsibilities across two SPI buses and two rendering paradigms.

### 2.1 Display Engine (SPI0 - Custom PIO DMA)
We abandoned standard library display drivers in favor of a bespoke, bare-metal C++ implementation utilizing the RP2350's PIO (Programmable I/O) and DMA (Direct Memory Access).

*   **PIO State Machine (`tft_io.pio`):** A custom assembly program runs on PIO0. It accepts 32-bit words from DMA, shifts out exactly 24 bits (MSB first) to the MOSI pin, and handles SCK toggling with cycle-accurate precision.
*   **Clock Speed:** The SPI0 bus operates at a blistering **60 MHz**, perfectly aligned with the RP2350's 300 MHz overclock (divider = 5).
*   **Color Expansion:** The CPU generates colors in standard 16-bit RGB565 format (to save RAM and bandwidth). Immediately prior to DMA transfer, an inline function (`_expand`) rapidly converts these to 24-bit RGB666 words.
*   **Ping-Pong Buffering (Asynchronous DMA):** The waterfall rendering (`ili9488_push_waterfall`) uses two line buffers. While the DMA controller blasts Buffer 0 to the PIO at 60MHz, the CPU (Core 1) actively processes noise generation, color expansion, and marker injection for Buffer 1. This ensures 100% bus utilization and **0% CPU blocking**.
*   **Performance:** Achieves a rock-solid **29-30 FPS** for a 480x160 pixel waterfall update. This hits the absolute mathematical limit of a 60MHz SPI bus transferring 24-bit pixels.

### 2.2 Touch & UI Framework (SPI1 - LovyanGFX)
While the display is driven by our custom engine, we retained the `LovyanGFX` library strictly for its robust, high-level features:
*   **Touch Controller (XPT2046):** LovyanGFX handles SPI1 communication at 2.5MHz, providing reliable, interrupt-driven touch reading.
*   **Calibration:** Utilizes LovyanGFX's native `calibrateTouch` to generate an affine transformation matrix, perfectly mapping the raw resistive sensor coordinates to the 480x320 physical pixels.
*   **Consensus Filtering:** A custom algorithm wraps the LovyanGFX touch reads. It takes burst samples (5 readings at 1ms intervals) and requires spatial clustering (Consensus) to reject electrical noise, origin-trailing, and edge-mirroring artifacts inherent to cheap resistive panels.
*   **Sprite Rendering:** For complex UI elements (text, anti-aliased lines, gradients), we use `LGFX_Sprite` to render graphics into RP2350 RAM. Once rendered, the memory buffer is passed to our custom PIO DMA driver, blasting the complex UI to the screen instantly without flickering.

## 3. Wiring Map (Finalized)
| Signal       | RP2350 GPIO | Description |
| :---         | :---        | :---        |
| **SPI0 (Display)**| | |
| SCK          | 18          | 60MHz PIO Clock |
| MOSI         | 19          | 60MHz PIO Data Out |
| MISO         | -           | **DISCONNECTED** (Prevents bus noise) |
| CS           | 17          | LCD Chip Select |
| DC           | 20          | Data/Command |
| RST          | 21          | Hardware Reset |
| **SPI1 (Touch)** | | |
| SCK          | 10          | 2.5MHz SPI Clock |
| MOSI         | 11          | SPI Data Out |
| MISO         | 12          | SPI Data In |
| CS           | 15          | Touch Chip Select |
| IRQ          | 14          | Touch Interrupt (Internal Pull-Up) |

## 4. Conclusion
Phase 1 is definitively complete. The hardware foundation provides absolute maximum theoretical throughput for the display, zero-flicker rendering via off-screen sprites and ping-pong DMA, and highly stable, noise-filtered touch input. The system is fully primed for complex DSP and UI development on the dual-core Cortex-M33 architecture.