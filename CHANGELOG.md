# Changelog: TouchRTTY (RP2350)

All notable changes to this project will be documented in this file.

## [Build 205] - 2026-04-05
### Added
- **Stop-bit popup**: 2×2 touch grid (1.0 / 1.5 / 2.0 / AUTO) with blue AUTO highlight
- **Auto stop-bit detection**: sequential test 1.0→1.5→2.0 (3s each), picks lowest ERR rate
- **Multi-signal SEARCH**: finds ALL RTTY signals on waterfall, cycles between them on repeat press
  - First press: selects strongest signal by score
  - Subsequent presses (< 10s): cycles through saved list without re-scanning
  - After 10s timeout: performs fresh search
- **SEARCH → AUTODETECT pipeline**: SEARCH triggers stop-bit detection + auto-inversion
- **Serial commands**: `STOP AUTO`, `STOP 0/1/2` for stop-bit control
- **Top bar indicators**: ST:1.5 (cyan), ST:1.5(A) (green auto), ST:.. (yellow detecting)
- **Bottom bar**: ST button shows current stop-bit or "ST:AUTO"

### Fixed
- **SEARCH not finding real signals**: candidates array overflow (32→128 with eviction), imbalance threshold too strict (10→20 dB), first press selected by frequency instead of score
- **1.0 stop-bit decoding ("123" → "0)")**: two root causes fixed:
  - Simulator ITA2 FIGURES table had `\03` octal escape bug (single ETX char instead of `\0`+`3`), fixed with `\x003`
  - Framer Continuous DPLL checked D polarity which failed due to biquad LPF delay; removed check for 1.0 stop bits
- **Serial console not responding to HELP**: VS Code Serial Monitor sends without CR/LF; added 500ms timeout-based command parsing
- **Auto stop-bit always picking 1.5**: test time too short (1s→3s), removed priority tie-breaker

### Changed
- **RTTY Simulator** (`tools/rtty_simulator.html`): shift dropdown (8 values + Custom), single center frequency input, auto-computed Mark/Space display, `setValueAtTime` for instantaneous frequency switching
- **Adaptive SEARCH threshold**: candidates scoring < 40% of best are discarded

## [Build 194] - 2026-04-04
### Added
- **Tuning Lab** (MENU → TUNE): dedicated screen for DSP parameter tuning
  - Eye diagram with phosphor persistence (240×64, DPLL-synchronized X axis)
  - Touch controls: ALPHA±, BW±, SQ± buttons
  - DUMP:ON/OFF toggle — enables continuous diagnostic stream to serial
  - SAVE button — writes all settings to flash
- **Serial Command System** (15 commands, type `HELP` for full list):
  - Tuning: `ALPHA`, `BW`, `SQ`, `FREQ`
  - Protocol: `BAUD`, `SHIFT`, `STOP`, `INV`
  - Control: `AFC`, `AGC`, `DIAG`, `STATUS`, `SAVE`, `CLEAR`
- **Diagnostic Stream** (`[D]` prefix, ~500ms interval):
  - SNR, SIG, ERR%, SQ state, AGC dB, DPLL phase/freq error, Mark/Space envelopes, core loads

### Changed
- **Menu restructure**: removed BW±, SQ±, SAVE from main menu (moved to Tuning Lab)
- **DIAG screen**: renamed DIAG:ON/OFF button to DUMP:ON/OFF
- **Boot encoder**: short press = touch recalibration only, long press (3s) = factory reset + recal

### Fixed
- Reset confirm dialog disappearing instantly (incoming RTTY chars overwrote text zone)
- Text zone flicker when Tuning Lab active
- Touch recalibration on boot: `shared_force_cal` was reset on early encoder release

## [Build 191] - 2026-04-04
### Added
- **Error rate indicator**: 100-character sliding window, displayed as percentage and bar in top panel
- **3 thin bars** in top panel: SIG (signal level), AGC (auto gain in dB), ERR (error rate %)
- **AGC display in dB** (right of AGC bar, replaces old multiplier display)

### Fixed
- **Reception broken** (Build 190): FFT on Core 0 blocked ADC for ~1ms → FIFO overflow → DPLL lost phase. Reverted FFT back to Core 1.
- **FPS drop 22→14** (Build 190): `__wfe()` in ADC wait loop didn't wake on ADC FIFO events. Fixed with `tight_loop_contents()`.
- **Core 1 at 90% load**: `tight_loop_contents()` idle loop counted as work. Fixed with `sleep_us(20)`.

## [Build 190] - 2026-04-04
### Added
- **Hardware ADC FIFO**: `adc_fifo_setup()` + `adc_run(true)` for jitter-free 10kHz sampling
- **Ping-pong double buffering** in `ili9488_push_colors()` for DMA transfers
- **fast_log2f()**: IEEE 754 bit-trick approximation (~4x faster than `log10f`)
- **AGC optimization**: precomputed `1/release` (multiply instead of divide)
- **Lissajous scope**: bitmask phosphor fade + sin/cos lookup table

## [Build 189] - 2026-04-02
### Optimized
- **Hardware FPU Acceleration:** Enforced strict `float` policy across all DSP code (Core 0).
- **Fast Math Migration:** Replaced all double-precision functions with single-precision `float` variants.
- **Performance Milestone:** Core 0 load reduced to ~7% at 10kHz sample rate.
- **Compilation Flags:** `-O3`, `-ffast-math`, `-funroll-loops` verified in CMake.

## [Build 188] - 2026-04-02
### Added
- Professional font system: NORM (Font2, 17px) and NARW (Font0, 10px).
- Pixel-perfect rendering (removed all fractional scaling).
- Hardware-accurate color rendering for ILI9488 (RC1.2).

## [Build 185] - 2026-04-01
### Added
- DIAG sub-menu with Zero Bias Meter, Rainbow Palette, line width control.
- Smart Newline (CR/LF collapsing for radio-teletype streams).
- AFC button in bottom bar.

## [Build 172] - 2026-03-25
### Added
- Continuous DPLL with PI controller for 1.0 stop-bit streams.
- Strict SNR-based squelch with hysteresis.
- Quadrature I/Q demodulator with Biquad LPF.
- 63-tap FIR bandpass filter.
- Baudot/ITA2 decoder with FIGS/LTRS support.
