# TouchRTTY (RP2350 / Raspberry Pi Pico 2)
**Professional-grade Radioteletype (RTTY) Decoder exclusively for the RP2350 microcontroller.**

<p align="center">
  <img src="docs/images/device_view_1.jpg" width="48%" />
  <img src="docs/images/device_view_2.jpg" width="48%" />
</p>

This project implements a high-performance, software-defined radio (SDR) style RTTY decoder on the **dual-core RP2350 (ARM Cortex-M33)**. It utilizes the advanced DSP capabilities of the RP2350 to achieve highly stable reception even under severe selective fading and noise.

> [!IMPORTANT]
> **Hardware Requirement:** This project is specifically designed for the **Raspberry Pi Pico 2 (RP2350)**. It will NOT run on the original RP2040 due to higher memory and DSP requirements.

## 🚀 Key Features (Phase 3 Complete)

*   **RP2350 Dual-Core Optimization:**
    *   **Core 0 (DSP Engine):** Dedicated strictly to hard-real-time audio processing at exactly 10,000 Hz using RP2350's floating-point unit (FPU).
    *   **Core 1 (UI & Rendering):** Handles the 3.5" ILI9488 TFT touch display via 60MHz PIO DMA, rendering a 30+ FPS Waterfall, Spectrum, and Lissajous (XY) tuning scope without interrupting the DSP.
*   **Professional DSP Pipeline (Build 189 RC3):**
    *   **63-Tap FIR Bandpass Filter:** Pre-filters the 10kHz ADC stream.
    *   **Quadrature (I/Q) Demodulator:** Baseband mixing with hardware-generated sine/cosine tables, followed by Biquad Low-Pass Filters (Extended Raised Cosine). Eliminates Inter-Symbol Interference (ISI).
    *   **Automatic Threshold Correction (ATC):** Fast-attack, slow-release (FASR) envelope detectors independently track Mark and Space fading, dynamically adjusting the decision threshold.
    *   **Digital Phase-Locked Loop (DPLL):** A full Proportional-Integral (PI) synchronous loop tracks the zero-crossings of the bitstream, correcting both phase (timing jitter) and frequency error (baud rate mismatch). Allows continuous, gapless reception of 1.0 stop-bit streams without framing errors.
    *   **Automatic Frequency Control (AFC):** 512-point FFT-based peak detection locks onto wandering signals within a ±100Hz window.
    *   **Strict Squelch:** Intelligent noise-floor and SNR tracking ensures the decoder remains totally silent until a valid RTTY signal (SNR > 4dB) is present.
    *   **Error Rate Monitoring:** Real-time framing error percentage tracking via a dynamically scaling window.
*   **Hardware Compatibility:**
    *   Optimized for the **ILI9488** display quirk (Mode 11: 16-bit Endian Swapped, BGR out), rendering pure, artifact-free colors via native RGB565 manipulation.

## 📅 Development Roadmap

*   **PHASE 4:** SD-Card Integration (exFAT) & Data Logging.
*   **PHASE 5:** CW (Morse Code) Decoder & APF Filter.
*   **PHASE 6:** FT8 / FT4 Mode Implementation.
*   **PHASE 7:** WEFAX Decoder (HF Weather Fax).

## 📡 WebSDR / Real-World Testing Guide

You can now test this decoder with real over-the-air signals using a WebSDR (like the University of Twente WebSDR)!

### Setup
1.  Open a WebSDR in your browser.
2.  Tune to a known RTTY frequency (e.g., German Weather Service DWD on `10100.8 kHz`).
3.  Set the WebSDR modulation to **USB** (Upper Sideband).
    *   *Note: DWD transmits in F1B/LSB. If you tune in USB, the Mark frequency will be higher than the Space frequency. You must press the `INV` button on the Pico's screen to swap them, OR tune the WebSDR to LSB and leave the Pico in Normal mode.*
4.  Connect your PC's headphone output using the audio adapter (read about it below) to the Pico's ADC input (GPIO 26) using an audio cable. 
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

## 🔌 Hardware Wiring Guide

The project utilizes the Raspberry Pi Pico 2 (RP2350) and a 3.5" ILI9488 TFT Display with an XPT2046 touch controller. Below is the required pinout mapping.

### Display (ILI9488) - SPI0
| Display Pin | Pico GPIO | Physical Pin | Function |
| :--- | :--- | :--- | :--- |
| **VCC** | - | Pin 36 | 3.3V Power (3V3_OUT) |
| **GND** | - | Pin 38 | Ground (GND) |
| **CS**  | GP17 | Pin 22 | LCD Chip Select |
| **RESET**| GP21 | Pin 27 | Hardware Reset |
| **DC/RS**| GP20 | Pin 26 | Data / Command |
| **SDI (MOSI)**| GP19 | Pin 25 | SPI Data In |
| **SCK** | GP18 | Pin 24 | SPI Clock (60 MHz) |
| **SDO (MISO)**| GP16 | Pin 21 | Define in code, physically disconnect to reduce bus noise |
| **LED** | - | Pin 36 | Backlight Power (3V3_OUT) |

### Touch Controller (XPT2046) - SPI1
| Touch Pin | Pico GPIO | Physical Pin | Function |
| :--- | :--- | :--- | :--- |
| **T_CLK** | GP10 | Pin 14 | SPI Clock (2.5 MHz) |
| **T_CS**  | GP15 | Pin 20 | Touch Chip Select |
| **T_DIN** | GP11 | Pin 15 | SPI TX (MOSI) |
| **T_DO**  | GP12 | Pin 16 | SPI RX (MISO) |
| **T_IRQ** | GP14 | Pin 19 | Interrupt (Boot Calibration) |

### Audio Input
| Component | Pico GPIO | Physical Pin | Function |
| :--- | :--- | :--- | :--- |
| **Audio Signal** | GP26 | Pin 31 | ADC0 (Biased Audio Input) |
| **Audio Ground** | - | Pin 33 | Analog Ground (AGND) |

### Rotary Encoder (Future UI Expansion)
*Currently, only the push-button (SW) is implemented to serve as a Hard Reset and UI interaction. Full rotary tuning (A/B pins) will be added in future phases.*
| Encoder Pin | Pico GPIO | Physical Pin | Function |
| :--- | :--- | :--- | :--- |
| **SW (Switch)** | GP4 | Pin 6 | Push Button to GND (Hold on boot to Factory Reset) |
| **CLK / A** | *TBD* | - | *Reserved for future use* |
| **DT / B**  | *TBD* | - | *Reserved for future use* |
| **GND** | - | Any GND | Ground |

### SD Card Module - SPI1 (Phase 5 Logging)
*The SD Card shares the **SPI1 bus** with the Touch Controller, but requires its own dedicated Chip Select (CS) pin.*
| SD Card Pin | Pico GPIO | Physical Pin | Function |
| :--- | :--- | :--- | :--- |
| **MOSI / CMD** | GP11 | Pin 15 | SPI1 TX (Shared with Touch T_DIN) |
| **MISO / D0**  | GP12 | Pin 16 | SPI1 RX (Shared with Touch T_DO) |
| **SCK / CLK**  | GP10 | Pin 14 | SPI1 Clock (Shared with Touch T_CLK) |
| **CS / DAT3**  | GP13 | Pin 17 | Dedicated SD Chip Select |
| **VCC** | - | Pin 36 or 40 | 3.3V or 5V (Depends on your SD module) |
| **GND** | - | Any GND | Ground |

## 🔌 Hardware Audio Input Adapter
To safely feed audio from a PC, radio, or WebSDR into the RP2350's ADC (Analog-to-Digital Converter), a simple DC-biasing circuit is required. The Pico's ADC reads voltages between **0V and 3.3V**, so an AC audio signal centered around 0V will clip and potentially damage the pin if negative voltages are applied.

**Required Circuit:**

![Hardware Audio Adapter Schematic](docs/images/adc_input_adapter.png)

1. **R1 (Input Level):** A 10kΩ potentiometer to adjust the audio volume from your source.
2. **C1 (DC Blocking):** A 4.7µF capacitor to block any DC offset from the PC or radio.
3. **R2 (Bias Voltage):** A 10kΩ trimpot connected between 3.3V (Pin 36) and AGND to pull the ADC's resting voltage to exactly **1.65V** (the center of the ADC's range).
4. **R3 + C2 (Low-Pass Filter):** A 1kΩ resistor and 47nF capacitor forming a simple RC low-pass filter. This suppresses high-frequency RF noise and anti-aliases the signal before it hits the ADC.

*Important:* For the cleanest reception with the lowest noise floor, connect all ground lines of this circuit to the Pico's **AGND (Analog Ground, Pin 33)** rather than a regular digital ground.

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

## 📜 Release History

*   **[v1.72](https://github.com/Alex-Electron/TouchRTTY/releases/tag/v1.72)** (2026-03-31): **Phase 3 Final.** Professional DSP demodulator stability, build 172, ili9488 driver refactoring.

## 🏗️ Build Instructions
Compiled via the standard Raspberry Pi Pico SDK (v2.2.0+) and CMake.

```bash
mkdir build && cd build
cmake -G Ninja ..
ninja
picotool load TouchRTTY.uf2
```