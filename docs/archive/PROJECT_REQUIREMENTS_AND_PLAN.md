# Autonomous RTTY/CW/FT8 Digital Radio Terminal
## Project Vision & Requirements
**Platform:** Raspberry Pi Pico 2 (RP2350)
**Goal:** Create a standalone, highly stable digital mode decoder matching the performance of professional SDR software like `fldigi` or `PhosphorRTTY`.
**Hardware:**
*   RP2350 Overclocked to 250MHz (VREG 1.20V).
*   2.8" SPI TFT Display (ILI9341) + XPT2046 Touch.
*   Custom Layer 0 Audio Input: 1.65V DC Bias, 10k potentiometer, 4.7uF Tantalum input coupling, 1k+43nF (3.7kHz) RC Anti-aliasing filter connected to ADC0 (GPIO 26).

## Core Directives & Architectural Mandates
1.  **Strict RP2350 Optimization:** Ignore RP2040 limitations. Aggressively utilize RP2350 hardware: Cortex-M33 DSP instructions (SIMD/MAC), Hardware FPU (Single Precision `float`), DMA (Ring Buffers, SPI TX), PIO, and Dual-Core architecture.
2.  **Professional-Grade Stability (fldigi standard):** The receiver must handle fading (QSB), noise, and drift perfectly, even when sourced from WebSDR via virtual cables.
    *   *Requirement:* Streaming I/Q or Matched FIR/IIR filters for Mark/Space isolation.
    *   *Requirement:* Differential envelope detection (Mark Power vs. Space Power) rather than simple amplitude thresholds.
    *   *Requirement:* Software Phase-Locked Loop (PLL) for precision bit-clock recovery and center-symbol sampling.
3.  **Language Policy:** Technical descriptions/explanations in Russian. Code comments and variables in English.
4.  **Documentation Standards:** Maintain all technical specs and diagrams in Markdown. Use simplified, high-compatibility Mermaid syntax (`graph TD`) for all architectural diagrams to ensure consistent rendering across all viewers.

## Development Roadmap & Phase Plan

### Phase 1: Hardware Foundation & Signal Visualization (Completed)
- [x] Configure ADC with Hardware DMA Ring Buffer (10kHz sample rate) to offload CPU.
- [x] Implement ILI9341 Driver via SPI0 with DMA asynchronous transfers (62.5MHz).
- [x] Enable Dual-Core processing: Core 0 (DSP) and Core 1 (UI/Graphics).
- [x] Implement Fast Radix-2 FFT utilizing Hardware FPU.
- [x] Create "Phosphor" Waterfall and Spectrum Scope UI.
- [x] Integrate XPT2046 Touchscreen (SPI sharing) for interactive frequency selection (Touch-to-Tune).

### Phase 2: RTTY Demodulation Engine (Current Objective)
- [ ] **Manual Parameter Structure:** Define `RTTYConfig` (Mark/Space freqs, Baud rate, Shift, Inversion) adjustable via UI.
- [ ] **Streaming DSP:** Implement dual matched filters (Goertzel or I/Q quadrature) for continuous Mark/Space power tracking at 10kHz.
- [ ] **Differential Detector:** Compare Mark vs. Space power to determine bit state, ensuring resilience to signal fading.
- [ ] **Bit Synchronizer (PLL):** Implement a software Phase-Locked Loop to track baud transitions and sample exactly at the bit center.
- [ ] **Baudot (ITA2) Decoder:** Translate 5-bit sequences into characters, handling LTRS/FIGS state shifts.
- [ ] **Output:** Display decoded text via USB-Serial (stdio) for initial validation.

### Phase 3: UI Integration & SD Card Storage
- [ ] **On-Screen Text:** Render decoded characters directly onto the ILI9341 display.
- [ ] **UI Controls:** Add touch buttons for Shift (170/425/850), Baud (45.45/50/75), and Reverse.
- [ ] **SD Card Integration:** Initialize the SPI SD card reader (FatFS). Ensure support for large-capacity SD cards using **exFAT** (requires enabling `FF_USE_EXFAT` in FatFS or using a compatible library).
- [ ] **Radiogram Archiving:** Implement functionality to automatically or manually save decoded RTTY/CW text streams into `.txt` or log files on the SD card for future analysis and record-keeping.

### Phase 4: Autonomous "Phosphor" Capabilities (The "Smart" Decoder)
- [ ] **Auto-Shift Detection:** Analyze the FFT to automatically identify the distance between the two strongest FSK peaks.
- [ ] **Auto-Baud Detection:** Apply spectral derivative analysis to the demodulated bitstream to identify the transmission speed.
- [ ] **Auto-Tracking:** Continuously adjust filter center frequencies to track drifting signals automatically.

### Phase 5: Wideband Multi-Channel CW Decoder (Future Ambition)
- [ ] **Concept:** Process the entire 2.5-3kHz audio passband simultaneously to decode multiple Morse code signals in parallel.
- [ ] **UI Redesign:** Replace the horizontal waterfall with a vertical frequency scale (peaks pointing right). Decoded text streams horizontally next to each detected signal peak.
- [ ] **DSP:** Deploy a grid of parallel detectors and auto-WPM estimators across the spectrum.

### Phase 6: WEFAX (Weather Fax) Decoder & Imaging
- [ ] **FM Demodulation:** Implement frequency-to-brightness conversion (1500Hz black to 2300Hz white) for grayscale imaging using I/Q phase differentiation.
- [ ] **Sync & Phasing:** Detect the 300Hz/450Hz phasing signals to align line starts and compensate for clock drift (Deskew).
- [ ] **High-Res Storage:** Save full-resolution scan lines (IOC 576, ~1810 pixels wide) directly to the SD card in `.PGM` or `.BMP` format.
- [ ] **On-Screen Preview:** Render a real-time downsampled preview of the weather map/satellite image on the TFT display.

## Context Notes
The RP2350 possesses immense computational headroom at 250MHz. Currently, the FFT and graphics routines consume less than 20% of the available processing power, leaving vast reserves for the complex PLLs, multi-channel I/Q filtering, and parallel CW decoding envisioned in Phase 5.