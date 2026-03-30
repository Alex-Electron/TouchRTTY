# TouchRTTY (RP2350)
**Professional-grade Radioteletype (RTTY) Decoder for Raspberry Pi Pico 2**

This project implements a high-performance, software-defined radio (SDR) style RTTY decoder on the dual-core RP2350 microcontroller. It utilizes advanced digital signal processing (DSP) techniques derived from professional modems like **fldigi** and the theoretical work of Kok Chen (W7AY), achieving highly stable reception even under severe selective fading and noise.

## 🚀 Key Features (Phase 3 Complete)

*   **Dual-Core Architecture:**
    *   **Core 0 (DSP Engine):** Dedicated strictly to hard-real-time audio processing at exactly 10,000 Hz.
    *   **Core 1 (UI & Rendering):** Handles the 3.5" ILI9488 TFT touch display via 60MHz PIO DMA, rendering a 30+ FPS Waterfall, Spectrum, and Lissajous (XY) tuning scope without interrupting the DSP.
*   **Professional DSP Pipeline:**
    *   **63-Tap FIR Bandpass Filter:** Pre-filters the 10kHz ADC stream.
    *   **Quadrature (I/Q) Demodulator:** Baseband mixing with hardware-generated sine/cosine tables, followed by Biquad Low-Pass Filters (Extended Raised Cosine). Eliminates Inter-Symbol Interference (ISI).
    *   **Automatic Threshold Correction (ATC):** Fast-attack, slow-release (FASR) envelope detectors independently track Mark and Space fading, dynamically adjusting the decision threshold.
    *   **Digital Phase-Locked Loop (DPLL):** A full Proportional-Integral (PI) synchronous loop tracks the zero-crossings of the bitstream, correcting both phase (timing jitter) and frequency error (baud rate mismatch). Allows continuous, gapless reception of 1.0 stop-bit streams without framing errors.
    *   **Automatic Frequency Control (AFC):** 512-point FFT-based peak detection locks onto wandering signals within a ±100Hz window.
    *   **Strict Squelch:** Intelligent noise-floor and SNR tracking ensures the decoder remains totally silent until a valid RTTY signal (SNR > 4dB) is present.
*   **Hardware Compatibility:**
    *   Optimized for the **ILI9488** display quirk (Mode 11: 16-bit Endian Swapped, BGR out), rendering pure, artifact-free colors via native RGB565 manipulation.
*   **Supported Modes:**
    *   Speeds: 45.45, 50, 75 Baud
    *   Shifts: 170, 200, 425, 450 (DWD SYNOP), 850 Hz
    *   Stop Bits: 1.0, 1.5, 2.0
    *   Polarity: Normal / Inverted (LSB standard: Mark is lower frequency)

## 📡 WebSDR / Real-World Testing Guide

You can now test this decoder with real over-the-air signals using a WebSDR (like the University of Twente WebSDR)!

### Setup
1.  Open a WebSDR in your browser.
2.  Tune to a known RTTY frequency (e.g., German Weather Service DWD on `10100.8 kHz`).
3.  Set the WebSDR modulation to **USB** (Upper Sideband).
    *   *Note: DWD transmits in F1B/LSB. If you tune in USB, the Mark frequency will be higher than the Space frequency. You must press the `INV` button on the Pico's screen to swap them, OR tune the WebSDR to LSB and leave the Pico in Normal mode.*
4.  Connect your PC's headphone output to the Pico's ADC input (GPIO 26) using an audio cable. 
    *   *Ensure your audio level is correct. Watch the top-left `SIG` meter on the screen; it should peak around `-15 dB` to `-5 dB` without triggering the red clipping indicator.*

### Tuning on the Pico
1.  **DWD SYNOP (Weather):** 
    *   Select **B 50** (50 Baud).
    *   Select **S 450** (450 Hz Shift).
    *   Select **ST 1.5** (1.5 Stop bits).
2.  **Amateur Radio (Ham):**
    *   Select **B 45** (45.45 Baud).
    *   Select **S 170** (170 Hz Shift).
    *   Select **ST 1.5** (1.5 Stop bits).
3.  Tap the Waterfall to place the yellow/cyan markers over the two visible peaks.
4.  The `RTTY: WAIT` indicator should turn green and say `RTTY: SYNC`.
5.  Text will begin printing on the screen!

## 🔌 Hardware Audio Input Adapter
To safely feed audio from a PC, radio, or WebSDR into the RP2350's ADC (Analog-to-Digital Converter), a simple DC-biasing circuit is required. The Pico's ADC reads voltages between **0V and 3.3V**, so an AC audio signal centered around 0V will clip and potentially damage the pin if negative voltages are applied.

**Required Circuit:**
1. **DC Blocking Capacitor:** Connect a 1µF to 10µF capacitor in series with the incoming audio signal to block any DC offset from the source.
2. **Voltage Divider (Bias):** Connect two identical resistors (e.g., 10kΩ or 100kΩ) to the ADC pin—one to 3.3V (Pin 36) and one to GND. This pulls the ADC's resting voltage to exactly **1.65V** (the center of the ADC's range).
3. **Low-Pass Filter (Optional but Recommended):** Add a small capacitor (e.g., 10nF to 100nF) between the ADC pin and GND to form a simple RC low-pass filter with the divider resistors. This suppresses high-frequency RF noise and anti-aliases the signal before it hits the 10kHz ADC.

*Connect the biased output to **GPIO 26 (Pin 31)**.*

## 🛠️ USB Serial Diagnostics (Tuning Mode)

By connecting the Pico via USB to a PC terminal (9600 baud), you gain access to the raw DSP telemetry every 500ms:

```
--- TUNING DIAGNOSTICS ---
Step 1 (ADC): V=1.94V (Range: 1585-2524)
Step 2 (ATC Level): Mark_Env=0.0974 Space_Env=0.0894
Step 3 (FFT Peaks): SNR=67.8 dB, Signal=-10.3 dB
Step 4 (RTTY Status): Squelch=OPEN DPLL_Phase=0.95
Params: Baud=45.45 ALPHA=0.0350 K=0.75 SQ=4.0
---------------------------------
```

You can send commands via the terminal to adjust the DSP on the fly:
*   `ALPHA 0.05` - Adjust DPLL tracking width (default 0.035).
*   `K 0.6` - Adjust Biquad LPF bandwidth multiplier (default 0.75).
*   `SQ 6.0` - Adjust Squelch SNR threshold (default 4.0).
*   `CLEAR` - Hard reset the DSP state, AFC, and DPLL phase.

## 🏗️ Build Instructions
Compiled via the standard Raspberry Pi Pico SDK (v2.2.0+) and CMake.

```bash
mkdir build && cd build
cmake -G Ninja ..
ninja
picotool load TouchRTTY.uf2
```