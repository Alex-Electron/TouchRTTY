# RTTY Decoder Phase 2: UI & DSP Foundation (Completed)

## Core Accomplishments
1. **Frankenstein DMA Ping-Pong Driver:** The SPI display driver is fully decoupled from CPU execution. PIO and DMA run at 60MHz, delivering ~30 FPS updates for the 480x160 DSP zone with zero screen tearing and near 0% CPU load on Core 0.
2. **Dual-Core Segregation:** Core 0 is strictly reserved for the 10kHz ADC polling loop, the 63-tap FIR Anti-Mains filter, and the 1024-point Radix-2 FFT. Core 1 handles the LGFX UI touch overlay, temporal smoothing (flicker reduction), and DMA kickoff.
3. **AGC and Dynamic Scale:** Implemented an `AUTO` scaling system that finds the peak magnitude and calculates an optimal dB span, adjusting the Noise Floor on the fly. Manual overrides (`FL-`, `FL+`, `GN-`, `GN+`) allow user intervention with true Decibel (dB) step metrics. `EXP/LIN` scale allows emphasizing small peaks without breaking the `AUTO` tracking loop.
4. **Zero Bias Hardware Meter:** Added a live-updating meter (top right) to physically aid the user in dialing the hardware trimpot to an exact 1.65V DC offset to maximize the ADC's dynamic range. The needle goes green when dialed perfectly within 50mV.
5. **Live SNR (Signal-To-Noise) Meter:** The core calculates true SNR by extracting the peak magnitude of the spectrum and comparing it to the average noise floor across the entire passband, displaying a live dB rating.
6. **Mathematical Marker Widths:** The RTTY Mark and Space cursors (`SH: 170`) are no longer hardcoded pixel widths, but are mathematically derived from the FFT bin width (7.18Hz per bin), ensuring they perfectly bracket the RTTY tones regardless of spectrum width.
7. **Unified Theme Engine (Build 111):** The UI now uses a persistent `THEME` button to toggle through three specific visual themes, decoupling the hardware ILI9488 pixel formatting from the logical color palettes.
   * `0: Blue Pastel` uses Driver Mode 4 (Native BRG) which accidentally, yet beautifully, blends color channels to create a soft, pastel visual. A white mix blend defaults to 1.0.
   * `1: Blue Bright` uses Driver Mode 6 (Swapped RGB) for true, mathematically accurate colors.
   * `2: Hacker Green` uses Driver Mode 6 (Swapped RGB) for a true matrix-style green.
8. **Color Blending:** The user can fine-tune the pastel softening level of any theme by connecting via USB UART and pressing `q` (decrease blend) or `w` (increase blend), dynamically shifting the pixel rendering.

## Action Items for Next Phase (Phase 3)
* Implement the Goertzel algorithm or a fast I/Q demodulator locked onto the `tune_x` coordinates from the UI marker.
* Calculate mark/space amplitudes and apply a comparator to generate a raw bitstream.
* Feed the bitstream into a Baudot state machine for decoding text directly onto the text zone sprite.