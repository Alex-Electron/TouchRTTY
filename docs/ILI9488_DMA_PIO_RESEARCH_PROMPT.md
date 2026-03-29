# RP2350 / ILI9488 SPI DMA & PIO Display Artifacts: Deep Analysis Request

## Context & Hardware
- **MCU:** Raspberry Pi Pico 2 (RP2350A), overclocked to 300MHz (VREG 1.30V).
- **Display:** 3.5" IPS TFT LCD with **ILI9488** controller (4-wire SPI mode).
- **Resolution:** 480x320.
- **Wiring:** Hardware SPI0 (GPIO 18 SCK, GPIO 19 MOSI, GPIO 17 CS, GPIO 20 DC).
- **Core Requirement:** The ILI9488 in 4-wire SPI mode *strictly requires* 18-bit color (RGB666), meaning we must send 3 bytes (24 bits) per pixel instead of the standard 16-bit (RGB565) 2 bytes.

## The Problem Statement
We are attempting to achieve maximum frame rate (60MHz+ SPI clock) while completely offloading the CPU using DMA to write directly to the display. We have encountered two distinct failure modes depending on the chosen peripheral (DMA via SPI vs. DMA via PIO).

We need a deep internet search and analysis of open-source solutions (e.g., Bodmer's `TFT_eSPI`, `LovyanGFX`, Murmulator implementations, Pico-SDK issues) to find the mathematically perfect, artifact-free way to drive this specific display at maximum speed using RP2350 hardware acceleration.

### Approach 1: Hardware SPI + 8-bit DMA
**Method:** We pack RGB565 into an array of 8-bit `uint8_t` values `[R, G, B, R, G, B...]` (where R is `r5 << 3`, etc.) and use `dma_channel_transfer_from_buffer_now` with `DMA_SIZE_8` pushing to the SPI TX FIFO (`spi_get_hw(SPI_PORT)->dr`).
**Result:** 
- Colors are perfectly vibrant and correct.
- CPU is 100% free.
- **The Bug:** When drawing partial screen updates (e.g., vertical rectangles or horizontal lines that do not span the full 480 pixels), the DMA/SPI FIFO synchronization occasionally slips. This causes the R, G, B byte sequence to misalign by 1 byte mid-transfer. From that point on, Green becomes Red, Blue becomes Green, etc., resulting in "color striping" and random noise artifacts on the right edge of the drawn region.

### Approach 2: Custom PIO State Machine + 32-bit DMA
**Method:** To solve the SPI FIFO alignment issue, we wrote a PIO program to manually shift out exactly 24 bits per pixel. We pack the 24-bit color into a 32-bit `uint32_t` and use `DMA_SIZE_32` to push to the PIO TX FIFO. The PIO program shifts out 24 bits and drops the remaining 8 bits.
**Result:**
- Alignment is perfect. No stripes, no right-edge artifacts regardless of the drawing window size.
- CPU is 100% free.
- **The Bug:** The colors are severely washed out ("pastel" or "dim"). We tested both `shift_right=true` (LSB first) and `shift_right=false` (MSB first) along with various C-level bit-packing permutations (e.g., `(r<<24)|(g<<16)|(b<<8)`, compensating for ARM Little Endianness). While we can get the hues correct (Red is Red, Green is Green), the *intensity* is heavily degraded. It appears the PIO shifting mechanism or phase (CPOL/CPHA) is causing the MSB of the color components to be dropped or misread by the ILI9488, cutting brightness by 50% or completely distorting the color space.

## Questions for AI Analysis:
1. **RP2350 DMA-to-SPI Alignment:** Is there a known erratum or specific configuration required on RP2040/RP2350 to prevent `DMA_SIZE_8` transfers to the hardware SPI FIFO from dropping/shifting bytes during rapid back-to-back CS toggles or partial window updates? How do libraries like TFT_eSPI handle continuous 24-bit streams via DMA without desyncing?
2. **PIO 24-bit Shifting (Washed Out Colors):** When using PIO to simulate SPI for an ILI9488 (which samples on the rising edge), what is the precise, cycle-accurate PIO assembly code required? Why would shifting 24 bits out of a 32-bit word cause a systematic loss of color intensity? Is it a setup/hold time violation on the MOSI pin relative to SCK, or a bit-endianness packing error?
3. **The "Holy Grail" Solution:** What is the accepted "best practice" in the RP2350 community for driving an ILI9488 over 4-wire SPI? Should we persist with debugging the PIO timing, or is there a trick to stabilizing the 8-bit hardware SPI DMA? Provide specific code snippets or PIO configurations from working projects.