# RTTY Decoder Phase 2: UI & DSP Foundation (Completed)

## Core Accomplishments
1. **Frankenstein DMA Ping-Pong Driver:** The SPI display driver is fully decoupled from CPU execution. PIO and DMA run at 60MHz, delivering ~30 FPS updates for the 480x160 DSP zone with zero screen tearing and near 0% CPU load on Core 0.
2. **Dual-Core Segregation:** Core 0 is strictly reserved for the 10kHz ADC polling loop, the 63-tap FIR Anti-Mains filter, and the 1024-point Radix-2 FFT. Core 1 handles the LGFX UI touch overlay, temporal smoothing (flicker reduction), and DMA kickoff.
3. **AGC and Dynamic Scale:** Implemented an `AUTO` scaling system that finds the peak magnitude and calculates an optimal dB span, adjusting the Noise Floor on the fly. Manual overrides (`FL-`, `FL+`, `GN-`, `GN+`) allow user intervention. `EXP/LIN` scale allows emphasizing small peaks by exponentiating normalized pixel heights.
4. **Zero Bias Hardware Meter:** Added a live-updating meter to physically aid the user in dialing the hardware trimpot to an exact 1.65V DC offset to maximize the ADC's dynamic range.
5. **Live Palette Tuner:** Added an active palette cycling feature (triggered by the `COLOR` UI button or via Serial terminal sending `0`-`3`) allowing live diagnosis of RGB/BGR display swapping issues and color preference selection without reflashing.

## Action Items for Next Phase (Phase 3)
* Implement the Goertzel algorithm or a fast I/Q demodulator locked onto the `tune_x` coordinates from the UI marker.
* Calculate mark/space amplitudes and apply a comparator to generate a raw bitstream.
* Feed the bitstream into a Baudot state machine for decoding text directly onto the text zone sprite.