# PIO ILI9488 Driver (Archive v1)

## Why we tried PIO:
The ILI9488 display controller in 4-wire SPI mode does not support the standard 16-bit (RGB565) color format. It strictly requires 18-bit (RGB666) format, meaning 3 bytes (24 bits padded) must be sent per pixel.

When using the RP2040/RP2350 hardware DMA to send 24-bit streams (using `DMA_SIZE_8` or `DMA_SIZE_32`), the asynchronous nature of the DMA and the SPI peripheral's FIFO caused synchronization glitches on the breadboard. If the SPI clock edges didn't perfectly align with the data boundaries, the RGB bytes shifted by a few bits (e.g., the last bit of Blue became the first bit of the next Red pixel). This manifested as horizontal, multicolored stripes (artifacts) across the screen.

To fix this, we implemented a custom PIO (Programmable I/O) state machine (`ili9488_spi.pio`). PIO allows absolute, cycle-accurate control over the SCK and MOSI pins. The DMA would feed 32-bit packed words to the PIO, and the PIO would shift out exactly 24 bits per pixel, completely bypassing the hardware SPI peripheral for bulk transfers.

## Why we are moving away from PIO (The "Washed Out Colors" Bug):
The PIO approach successfully eliminated the horizontal stripe artifacts. However, a new, critical issue arose: **Washed out, dim colors.**

This occurred because of Endianness and Bit Order translation:
1.  **ARM Core (Little Endian):** Stores 32-bit integers backwards in memory (`0xAABBCCDD` becomes `[DD] [CC] [BB] [AA]`).
2.  **DMA Transfer:** Copies the 32-bit word directly into the PIO TX FIFO.
3.  **PIO Shift (`out pins, 1`):** Needs to shift out the Most Significant Bit (MSB) of the *color byte* first, as required by the ILI9488 SPI spec.

While we managed to get the *bytes* (Red, Green, Blue) in the correct order by reversing them in C before packing, the PIO `out pins, 1` command with `shift_right=true` (LSB first) or `shift_right=false` (MSB first) applied globally to the entire 32-bit word. 

When we tried `shift_right=true` (LSB first), it sent the bits of each byte backwards (`10000000` became `00000001`). This caused maximum intensity colors (0xFF) to become minimum intensity colors (0x01), resulting in the extremely dark/pastel/washed-out colors seen in the photos. Attempting to fix this by enabling Hardware Inversion (`0x21`) on the display "fixed" the colors but introduced terrible backlight bleed and gradient issues, proving it was a bit-order software problem, not a hardware requirement.

## The New Strategy: Synchronous SPI or DMA with Pre-packed Bytes
We will abandon the complex 32-bit PIO packing and return to the hardware SPI peripheral. To prevent the DMA stripe artifacts, we will:
1.  Use `spi_write_blocking` (Synchronous SPI) for drawing small UI elements and text. The CPU is fast enough at 300MHz that waiting for the SPI FIFO is acceptable for non-fullscreen updates.
2.  If DMA is needed for the waterfall, we will use an array of explicitly packed `uint8_t` (8-bit array) and `DMA_SIZE_8` transfers. This forces the DMA to respect byte boundaries and allows the hardware SPI to handle the MSB-first bit shifting natively, guaranteeing perfectly bright, correct colors.