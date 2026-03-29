# TECHNICAL SPECIFICATION (COMBINED & REFINED)
# Autonomous Professional RTTY/CW/FT8 Digital Radio Terminal
## Version: 3.1 (Refined for Breadboard & RP2350 Overclock)
## Platform: Raspberry Pi Pico 2 (RP2350) + ILI9341 320x240 Touch

---

## 1. PROJECT VISION & CORE MANDATES
The device is a standalone digital mode decoder matching the stability and performance of professional SDR software (e.g., `fldigi`).
It processes real-time audio, decodes RTTY, and provides a rich touch-based UI with a live waterfall and parallel decoding.

**Strict Mandates:**
- **"fldigi" Stability:** Requires I/Q (Quadrature) envelope detection, matched FIR/IIR filters, and a Software PLL (Phase-Locked Loop) for precision bit-clock recovery.
- **Hardware Exploitation:** Aggressively utilize RP2350 features: Cortex-M33 DSP instructions (SIMD/MAC), FPU, dual-core architecture, and DMA ring buffers.
- **Architecture:** Core 0 handles all DSP (ADC DMA, FIR, PLL, Decoding). Core 1 handles UI (SPI LCD, Waterfall, Touch).

---

## 2. HARDWARE & WIRING (BREADBOARD OPTIMIZED)

### 2.1 Component Models (Explicit Specifications)
To prevent hardware confusion, the system is strictly tailored for the following exact components:
- **Microcontroller:** Raspberry Pi Pico 2 (MCU: RP2350A, Dual ARM Cortex-M33).
- **Display Module:** 3.5-inch IPS TFT LCD, Resolution: 480x320.
  - **Display Controller:** **ILI9488** (Requires 18-bit RGB666 color over SPI; does *not* support 16-bit RGB565 via 4-wire SPI).
  - **Touch Controller:** **XPT2046** (Resistive touch controller, standard SPI interface).
  - **SD Card Reader:** Integrated standard SPI SD/SDHC/SDXC slot.
  - **Onboard Power:** Module includes a 662K 3.3V Low-Dropout (LDO) regulator.

### 2.2 Microcontroller Configuration
- **MCU:** RP2350 (Overclocked to 300 MHz via `set_sys_clock_khz`, VREG 1.30V).

### 2.3 Analog Input (ADC) - CRITICAL FOR STABILITY
- **Signal Path:** Audio (Line Level) -> 10k Potentiometer (Level Shift to 1.65V) -> RC Low-Pass Filter (1kΩ + 43nF, Fc ≈ 3.7kHz) -> **GPIO 26 (ADC0)**.
- **Hardware Requirement:** To prevent noise, a **0.1µF + 10µF capacitor** MUST be placed between `ADC_VREF` (Pin 35) and `AGND` (Pin 33).
- **ADC Configuration:** 10,000 Hz sample rate (maintained for calculation simplicity vs 9600Hz), hardware DMA ring buffer.
### 2.3 Display, Touch & SD Card Wiring (LovyanGFX Dual SPI Architecture)
**Hardware Profile:** 3.5-inch IPS TFT, 480x320 Resolution, **ILI9488** Controller.

#### Verified Display Configuration (Perfect Colors):
To achieve vibrant, correct colors without artifacts or "washing out," the following parameters are strictly required:
- **Color Format:** 18-bit RGB666 (3 bytes per pixel: R, G, B).
- **Transfer Method:** LovyanGFX PIO DMA (60MHz).
- **MISO Connection:** **NOT REQUIRED.** The display MISO pin should be left DISCONNECTED to prevent bus noise.
- **MADCTL (0x36):** `0x48` (Raw Landscape orientation, BGR=0).
- **Display Inversion (0x20/0x21):** `ON` (0x21). 
- **Bit Alignment:** LovyanGFX handles 5->6 bit expansion and 24-bit alignment natively.

| Signal       | GPIO (Pico) | Physical Pin | Description |
| :---         | :---        | :---         | :---        |
| **Display Bus (SPI0 + PIO0)**| |          | *Transmit-only high-speed bus* |
| SCK (LCD)    | GPIO 18     | 24           | SPI0 SCK |
| MOSI (LCD)   | GPIO 19     | 25           | SPI0 TX |
| **MISO (LCD)**| **N/A**     | **-**        | **DO NOT CONNECT** |
| CS (LCD)     | GPIO 17     | 22           | Chip Select for Display |
| DC           | GPIO 20     | 26           | Data/Command (Display) |
| RST          | GPIO 21     | 27           | Reset (Display) |
| **Touch/SD Bus (SPI1)** |     |              | *Shared 2.5MHz SPI* |
| T_CLK / SD_CLK| GPIO 10     | 14           | SPI1 SCK |
| T_DIN / SD_DI | GPIO 11     | 15           | SPI1 TX |
| T_DO / SD_DO  | GPIO 12     | 16           | SPI1 RX (MISO for Touch/SD) |
| T_CS         | GPIO 15     | 20           | Chip Select for Touch Controller |
| SD_CS (SCS)  | GPIO 13     | 17           | Chip Select for SD Card Reader |
| T_IRQ        | GPIO 14     | 19           | Touch Interrupt (Requires internal `gpio_pull_up(14)`) |
| **Power (CRITICAL)**|        |              | |
| VCC/VDD      | VBUS (5V)   | 40           | **MUST be 5V** (Module has onboard 662K 3.3V LDO) |
| LED (BL)     | 3.3V(OUT)   | 36           | Connect directly or via 10-47Ω resistor for backlight |
| GND          | GND         | 38 / Any GND | Shared Ground |

### 2.4 Touch Calibration & Filtering (LovyanGFX Native + Consensus Filter)
While LovyanGFX provides an excellent built-in `calibrateTouch` routine that natively aligns the touch matrix to the rendering engine, the physical XPT2046 resistive layer on this specific panel exhibits significant electrical noise. This manifests as:
1. **Origin Trailing:** Sudden, noisy `(0,0)` or corner readings during the initial milliseconds of a touch (before contact resistance stabilizes).
2. **Edge Mirroring:** Erroneous, inverted coordinate jumps when the stylus crosses the physical bezel boundaries (especially on the right edge).

**Current Mitigation Strategy (Consensus Filtering):**
To combat this, the UI loop implements a multi-stage noise rejection algorithm:
- **Burst Sampling:** The system takes 5 rapid samples (1ms intervals).
- **Cluster Detection:** It requires at least 3 of those samples to fall within a tight spatial cluster (e.g., 15-pixel radius) to be considered a valid touch. Single-sample spikes are completely ignored.
- **Jump Filtering:** Even if a cluster is found, any sudden jump exceeding a reasonable distance (e.g., 50 pixels since the last frame) breaks the continuous drawing line to prevent long "mirroring" artifacts.

*Note for Future Development:* While this drastically improves usability and mimics legacy mobile touch stability, the filtering parameters (sample count, cluster distance, jump threshold) may require further empirical refinement during Phase 2 (UI Development) to find the perfect balance between responsiveness (drawing speed) and stability.

---

## 3. UI ARCHITECTURE: "FLDIGI-STYLE" WATERFALL & TOUCH

### 3.1 Screen Layout (480x320 Landscape)
- **Top (16px):** Status Bar (Baud, Shift, SNR).
- **Middle (128px):** Waterfall (FFT Scope, 300Hz - 3300Hz). Width expanded to 480px.
- **Middle (16px):** Signal Info Panel (Live Frequencies, Mark/Space Levels).
- **Bottom (128px):** Decoded Text Area.
- **Bottom (32px):** Toolbar (TUNE, AFC, Speed, Menu).

### 3.2 The Three Markers (Mark, Space, Center)
- **Visuals:** Space (Cyan Line), Center (White Dashed Line), Mark (Yellow Line). Green tinted blend area between Space and Mark.
- **Interactions (The "Smart Tap"):**
  1. User taps anywhere near a visible signal on the waterfall.
  2. System searches for the nearest peak.
  3. System automatically searches for a secondary peak at common shift intervals (170Hz, 425Hz, 850Hz).
  4. Markers snap to the found pair automatically.

### 3.3 TUNE Algorithm (Parabolic Interpolation)
To achieve sub-Hz precision from a coarse FFT (e.g., 20Hz/bin):
1. Locate the peak bin in the FFT magnitude array.
2. Apply Parabolic Interpolation using the peak bin and its two immediate neighbors:
   `offset = 0.5 * (y0 - y2) / (y0 - 2*y1 + y2)`
3. The true frequency is `(peak_bin + offset) * bin_hz`.

### 3.4 AFC (Automatic Frequency Control)
- Continuous tracking of the drifting signal.
- Updates marker positions (and DSP center frequencies) slowly (e.g., max 2Hz per step) only when active decoding is occurring (SNR > Threshold).

---

## 4. DSP PIPELINE (CORE 0)

1. **DMA Ring Buffer:** Receives 12-bit ADC samples continuously at 10kHz.
2. **Hanning Window & FFT:** Processes chunks for the Core 1 Waterfall.
3. **I/Q Demodulation (Envelope Detection):**
   - Incoming signal multiplied by `sin()` and `cos()` of Mark/Space frequencies (using precalculated LUTs or fast approx).
   - Low-Pass Filter applied to I and Q components.
   - Power calculated: `P = I^2 + Q^2`.
4. **Differential Comparator:** Mark Power vs Space Power -> Raw Bitstream.
5. **Software PLL (Bit Synchronizer):**
   - Tracks zero-crossings in the bitstream.
   - Adjusts the sampling phase to read the bit exactly at the center of the symbol period.
6. **Baudot Framer:** Translates 5-bit sequences into characters, handling LTRS/FIGS state.

---

## 5. PHASE IMPLEMENTATION PLAN (UPDATED)

- **Phase 1: Breadboard Hardware Bring-up (COMPLETED):** 
  - Integrated LovyanGFX for 60MHz PIO DMA rendering of 18-bit RGB666.
  - Resolved hardware inversion (0x21) and layout (480x320) quirks of the ILI9488 module.
  - Implemented dual-SPI architecture (SPI1 for touch) and a robust Consensus Touch Filter to eliminate resistive noise and edge artifacts. 
  - Hardware foundation is rock solid and CPU load is minimal.
- **Phase 2: UI Foundation (Next):** Implement fonts, the 3-marker waterfall, and layout structure.
- **Phase 3: DSP Engine:** Implement Parabolic Interpolation TUNE and I/Q Demodulator.
- **Phase 4: SD Card (exFAT):** Add logging of decoded text.
- **Phase 5: Wideband CW:** Parallel processing of the 3kHz passband.