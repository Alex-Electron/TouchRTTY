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
- **Signal Path:** Audio Input (Line/Headphones) -> 4.7µF DC-blocking capacitor -> 10k Potentiometer (acting as a voltage divider to shift the signal to a 1.65V center bias from the 3.3V rail) -> RC Low-Pass Filter (1kΩ + 43nF to Ground, Fc ≈ 3.7kHz) -> **GPIO 26 (Physical Pin 31, ADC0)**.
- **Audio Ground (CRITICAL):** The ground sleeve of the audio cable coming from the receiver MUST be connected strictly to the **Analog Ground (AGND - Physical Pin 33)** on the Pico 2. Do not connect the audio ground to the digital/power ground plane to prevent digital switching noise (especially from the 60MHz display SPI) from bleeding into the sensitive ADC.
- **Hardware Decoupling (Pico 2 Board Mods):** To achieve professional SDR noise floors, you MUST add external filtering capacitors directly to the Pico 2 pins:
  - Place a **0.1µF (ceramic) AND a 10µF (electrolytic)** capacitor in parallel directly between `ADC_VREF` (Pin 35) and `AGND` (Pin 33). This stabilizes the internal ADC reference voltage against VBUS fluctuations.
- **ADC Configuration:** 10,000 Hz sample rate (maintained for calculation simplicity vs 9600Hz), hardware direct polling loop in Core 0 (bypassing DMA to guarantee synchronous execution with DSP).
### 2.3 Display & Rendering Architecture ("Frankenstein" Model)
**Hardware Profile:** 3.5-inch IPS TFT, 480x320 Resolution, **ILI9488** Controller.

#### The Rendering Engine (Custom 60MHz PIO + DMA Ping-Pong):
Due to the strict 18-bit (RGB666) requirement of the ILI9488 module over 4-wire SPI, standard libraries like LovyanGFX default to a software fallback on the RP2350 architecture, resulting in unacceptable frame rates (~3 FPS).

To achieve professional-grade SDR waterfall performance, the system utilizes a custom-built, highly optimized **"Frankenstein" Architecture**:
1. **Graphics Rendering:** A bespoke PIO assembly program (`ili9488_spi.pio`) drives the SPI0 bus at **60 MHz**, natively expanding 16-bit RGB565 color data into the required 24-bit stream on the fly.
2. **Ping-Pong DMA:** The waterfall rendering function (`ili9488_push_waterfall`) utilizes double buffering. The Cortex-M33 CPU (Core 1) prepares the next horizontal line of noise/signal data while the DMA controller simultaneously blasts the current line to the display.
3. **Flicker-Free Overlays:** Tuning markers are injected dynamically into the DMA buffer *before* transmission, entirely eliminating the screen tearing and flickering associated with traditional overdraw methods.

**Performance Metric:** This custom engine achieves a stable **29-30 FPS** for a continuous 480x160 pixel waterfall update, representing the absolute theoretical mathematical maximum throughput of the 60MHz SPI bus (~31ms per frame for 1.84 million bits).

**MADCTL (0x36):** `0x28` (Native Landscape, MV=1, BGR=1).
**Display Inversion (0x21):** `ON` (Required for accurate black/white rendering on this specific IPS panel).
- **MISO Connection:** **NOT REQUIRED.** The display MISO pin should be left DISCONNECTED to prevent bus noise.

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
| **Hardware Expansion (Reserved)**| | | |
| Enc A        | GPIO 2      | 4            | Rotary Encoder Phase A |
| Enc B        | GPIO 3      | 5            | Rotary Encoder Phase B |
| Enc SW       | GPIO 4      | 6            | Rotary Encoder Push Button |
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

### 2.5 Audio Output (External DAC - Future Phase 6)
While the RP2350 lacks a built-in hardware DAC, Phase 6 (Audio DSP & Noise Reduction) will require high-fidelity analog audio output for monitoring filtered CW/RTTY tones. The system is designed to seamlessly integrate an external I2C DAC:
- **Module:** MCP4725 (12-bit I2C DAC Breakout Board).
- **Wiring (Proposed):**
  - **VCC:** 3.3V (Physical Pin 36)
  - **GND:** GND
  - **SDA:** GPIO 4 (Physical Pin 6) - Maps to hardware `I2C0 SDA`
  - **SCL:** GPIO 5 (Physical Pin 7) - Maps to hardware `I2C0 SCL`
- **Audio Routing:** The analog `OUT` pin of the MCP4725 should be connected to a high-impedance headphone input or a dedicated external audio amplifier module (e.g., LM386) to drive a speaker.

---

## 3. UI ARCHITECTURE: "FLDIGI-STYLE" WATERFALL & TOUCH

### 3.1 Screen Layout (480x320 Landscape)
- **Top (48px):** Status Bar (Baud, Shift, SNR, Live ADC Voltage, FPS, Frequency).
- **Upper Middle (112px):** DSP Zone (Oscilloscope & FFT Spectrum, 50Hz - 3500Hz).
- **Lower Middle (112px):** Decoded Text Area. Features fast, hardware-accelerated touch-scrolling (swipe up/down) to review history effortlessly.
- **Bottom (48px):** Flat Design Toolbar (TUNE, AFC, SPEED, CLEAR, MENU).

### 3.2 Interactions & Gestures
- **The "Smart Tap":**
  1. User taps anywhere near a visible signal on the waterfall.
  2. System searches for the nearest peak.
  3. System automatically searches for a secondary peak at common shift intervals (170Hz, 425Hz, 850Hz).
  4. Markers snap to the found pair automatically.
- **Text Scrolling:** Vertical swipe gestures within the Decoded Text Area enable rapid scrolling through historical received text.
- **Hardware Controls:** A rotary encoder (reserved on GPIO 2, 3, 4) will provide a tactile alternative for fine-tuning frequencies and navigating menus.

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
2. **Pre-Processing (Filters & Squelch):**
   - **DC Blocker:** An IIR High-Pass filter mathematically zeroes out any static DC offset (from the 1.65V bias or ground loops).
   - **FIR Bandpass Filter (63-Tap):** A custom 63-tap FIR filter (generated with a Hamming window) provides a "brick-wall" roll-off outside the 200Hz - 3200Hz human speech/digital mode passband. This completely eliminates 50Hz/60Hz mains hum and out-of-band ultrasonic noise before it hits the FFT.
3. **Hanning Window & FFT:** Processes chunks for the Core 1 Spectrum/Waterfall.
   - **Absolute Squelch AGC:** The UI rendering engine enforces a strict `[-10dB]` maximum gain clamp. If the input signal (or thermal noise) is entirely below the -60dB display floor, the screen renders perfectly flat, mimicking the absolute squelch of professional SDRs.
4. **I/Q Demodulation (Envelope Detection):**
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
- **Phase 6: Audio DSP & Noise Reduction (Future Expansion):** 
  - **Hardware:** Integration of an external 12-bit DAC (e.g., MCP4725 via I2C) using spare RP2350 GPIO pins.
  - **Features:** Real-time audio output for headphones/speakers. Implementation of CW Peaking (ultra-narrow 50Hz FIR/IIR filters) to pull Morse code out of the noise floor.
  - **Advanced DSP:** Spectral subtraction (FFT -> Noise Masking -> IFFT) and Adaptive LMS Noise Reduction for SSB/voice signals, leveraging the Cortex-M33's FPU and DSP instructions on Core 0.