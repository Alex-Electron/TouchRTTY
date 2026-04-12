# Changelog: TouchRTTY (RP2350)

All notable changes to this project will be documented in this file.

## [Build 221] - 2026-04-12
### Added
- **Seqlock для shared DSP data**: Core 0 оборачивает запись `shared_fft_ts/adc_waveform/mag_m/mag_s` в инкремент `shared_dsp_seq` с `__dmb()` барьерами. Core 1 читает с retry-циклом (до 3 попыток) — если seq изменилась между началом и концом memcpy, данные считаются рваными и перечитываются. Задел под будущий перенос FFT на Core 0 (частота shared-обновлений вырастет).
- **SAVE flash serial indicator**: `[SAVE] writing flash (DSP paused ~45ms)...` + `[SAVE] done in X us`. Кнопка SAVE в UI уже меняет цвет визуально.

### Changed
- Memory barriers `__dmb()` добавлены в Core 0 writer и Core 1 reader для корректной работы seqlock на двухъядерном ARM.

## [Build 220] - 2026-04-12
### Optimized
- **FIR 63-tap симметричный**: буфер power-of-2 (64) для bitmask-индексации вместо `% 63`. Использована симметрия коэффициентов (`fir_coeffs[i] == fir_coeffs[62-i]`) — 32 умножения + 31 сложение пар вместо 63 умножений. Forward iteration убирает reverse branch.
- FIR ~50% быстрее, освобождает ~0.5% Core 0.

## [Build 219] - 2026-04-12
### Added
- **PIO Waterfall LUT**: предвычисленная `waterfall_pio_lut[256]` таблица rainbow-gradient (uint8 → 32-bit PIO-ready RGB666). Rainbow-расчёт теперь O(1) lookup вместо 6 float-операций + color565 + byte swap на каждый из 480×64 = 30720 пикселей в кадре.
- **Circular history buffer**: `wf_history[64][480]` uint8 (30 KB) вместо RGB565 sprite (61 KB). Скролл = декремент `wf_offset` без memcpy.
- Новая функция `ili9488_push_waterfall_lut()` — рендер через history + LUT + ping-pong DMA.

### Changed
- Core 1 нижняя граница загрузки: 60% → **39%**. FPS водопада: стабильно 22 → 20-25.
- Reference идея из `c:\YandexDisk\DIY\RP2350_RTTY\TouchRTTY\` портирована (там та же схема PIO LUT + history buffer).

### Documented
- `docs/ROADMAP_OPTIMIZATION.md` раздел 8: гибридный декодер RTTY (цель — **лучше 2Tone**, порог ~−15..−16 дБ SNR). 4 этапа: Goertzel matched filter → Multi-phase Goertzel → Character-level ML → Bayesian prior + Viterbi + noise blanker + spectral sub + temporal diversity + tiny NN fallback + soft confidence UI.
- `docs/20260412/` — детальный анализ алгоритмов (RTTY_DECODER_ALGORITHMS_COMPARISON, IQ_VS_GOERTZEL_ML_ANALYSIS, OPTIMIZATION_AND_INTERFERENCE_MITIGATION).

## [Build 218] - 2026-04-12
### Added
- **Chain BAUD→STOP detection** (Build 217): STOP-DET now waits for BAUD-DET to complete before starting. New flag `shared_chain_stop_after_baud` ensures STOP gap classification uses the correct baud rate instead of a stale default.
- **STOP-DET warmup** (Build 218): first 1.5s of gap measurements are discarded — DPLL phase noise is too high immediately after framer switches to permissive mode.
- **STOP-DET idle filter** (Build 218): gaps > 1.25T are rejected as inter-frame pauses (previously counted as bin=2 votes, corrupting results).
- **Parabolic peak interpolation** (Build 216): sub-bin FFT precision for SEARCH frequency measurement. Center frequency accuracy improved from ±10 Hz to ±2-5 Hz.
- **Shift-proportional dedup tolerance** (Build 216): `max(3, shift_bins/8)` — prevents FSK spectral smearing from generating multiple false candidates for wide shifts (850 Hz: 6→1 candidate).
- **Clipping indicator** (Build 216): SIG bar blinks red/white with "CLIP!" text when ADC clips. 1.5s latch.
- **Auto-recovery chain** (Build 217): ERR > 15% for 3s triggers BAUD-DET → STOP-DET re-measurement.
- **Simulator Mark frequency mode** (Build 216): `rtty_simulator.html` now accepts both Center and Mark frequency as input.
- **serial_cmd.ps1 improvements** (Build 217): try/finally/Dispose for proper COM port cleanup; DTR/RTS enabled for USB CDC reads.

### Changed
- **STOP-DET bin boundaries** (Build 218): adjusted from 0.25/0.75 to 0.25/0.85 based on empirical gap measurements across all baud rates. 2.0 stop bits now correctly detected (gap ≈ 1.0T → bin 2).
- **SEARCH dist_penalty** increased from 1.5 to 2.5 for better shift discrimination (425 vs 450 Hz).
- **SEARCH pipeline**: when both BAUD and STOP are AUTO, only BAUD-DET fires; STOP-DET chains after completion (was: both fired in parallel, causing stale-baud misclassification).

### Fixed
- **STOP-DET wrong on 100 baud**: gap_fraction was computed with default baud (45.45) instead of detected baud. Fixed by chain logic.
- **STOP-DET always voting 2.0 for inter-frame pauses**: 54ms idle gaps (5.5T) were not filtered, all landed in bin=2. Fixed by 1.25T upper filter.
- **SEARCH cycle-leak** (Build 215): `found_current` from previous test caused entry into cycle path instead of full rescan. Removed cycle-by-frequency path after full rescan.
- **COM port phantom locks**: serial_cmd.ps1 had no try/finally, killed processes left phantom port locks.

### Documentation
- Full rewrite of `DEVELOPMENT_CONTEXT.md` — all algorithms, architecture, test results
- Full rewrite of `PHASE3_RTTY_DSP_FINAL.md` — detailed DSP/DPLL/SEARCH/BAUD-DET/STOP-DET
- Updated `ROADMAP_OPTIMIZATION.md` — refactoring history, performance optimizations, current status

### Tested
- **Simulator matrix (8/8 pass)**: 45/170, 50/450, 75/425, 100/850 × stop 1.0, 1.5, 2.0
- **Real signals via WebSDR (3/3 pass)**:
  - 4583 kHz DWD: 50/450/1.5 — clean decode
  - 10100 kHz DWD: 50/425/1.5 — correct with noise
  - 7646 kHz DWD: 50/450/1.5 — noisy but correct
  - 12579 kHz SITOR-B: 100/170 detected correctly (Baudot decoder N/A for FEC)

## [Build 206] - 2026-04-05
### Added
- **Baud rate auto-detection**: symbol duration histogram approach (like PhosphorRTTY)
  - Accumulates D-sign transitions for 3 seconds, builds interval histogram
  - Scores each candidate baud (45.45/50/75/100) by matching peaks at multiples of bit_period
  - Weighted scoring: distance decay + harmonic multiplier
  - Clear winner (>1.5× second best): apply immediately
  - Ambiguous: sequential ERR verification (2s per baud)
- **100 Baud support**: new baud rate for NAVTEX/SITOR
  - Baud popup: 3×2 grid (45/50/75/100/AUTO)
  - Serial command: `BAUD 0-3` (manual) or `BAUD 4`/`BAUD AUTO`
  - `shared_baud_idx`: 0=45, 1=50, 2=75, 3=100, 4=AUTO
- **BD indicator in top bar** (Row 3, under shift): BD:45 (cyan), BD:50(A) (green auto), BD:.. (yellow detecting)
- **100 Baud in test generator** (`tools/rtty_simulator.html`)

### Fixed
- **SEARCH not finding 450Hz meteo signal**: was only scanning manual shift; now always scans ALL 8 shifts
- **SEARCH breaking manual settings**: was forcing all params to AUTO; now only triggers auto-detect for params already in AUTO mode
- **SEARCH always applies detected shift**: switches shift_idx to AUTO after applying found shift

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
