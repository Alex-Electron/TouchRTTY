# Roadmap: optimization and improvement (Phase 3+)

> 🇷🇺 [Читать на русском](ROADMAP_OPTIMIZATION.ru.md)

*Updated: 2026-05-12, **Build 265 / v2.0.0** released.*

> **Status:** Phase 9 went to production along with the TinyML NN
> classifier. The strategic goal of section 8 ("better than 2Tone") is
> **achieved** — multi-run AWGN bench shows TouchRTTY beating 2Tone by
> 3–6× on real CER at SNR −12..−22 dB. The path turned out different
> from the originally planned route (dual-IQ + LLR instead of Goertzel
> + Character-ML), but the result is the same. Details in section 8
> below.

## 0. Code refactoring (Build 189-206) — DONE

### Splitting main.cpp into modules — DONE

Up to Build 189, everything (DSP, UI, serial, touch, state machines)
lived in one `main.cpp` (~1843 lines). As the project grew it was
broken into modules:

| File | Lines | Purpose |
|------|------:|---------|
| `main.cpp` | 55 | Entry point, HW init, Core 1 launch |
| `dsp_pipeline.cpp` | 703 | Core 0: ADC → AGC → I/Q → LPF → ATC → DPLL → Baudot → BAUD-DET → STOP-DET → auto-INV → auto-recovery |
| `ui_loop.cpp` | 915 | Core 1: FFT, SEARCH, spectrum/waterfall, touch, serial parser |
| `serial_commands.cpp` | 223 | 40+ serial commands (B265) |
| `settings_flash.cpp` | 103 | Reading/writing AppSettings to flash (2 MB offset) |
| `app_state.hpp/.cpp` | 144+96 | All shared volatile variables and constants |
| `ui/UIManager.hpp` | 1100+ | Rendering: spectrum, waterfall, text, top/bottom bar, menu, Tuning Lab |
| **Total** | **~3500** | |

### Refactoring principles

1. **Core separation:** `dsp_pipeline.cpp` runs strictly on Core 0,
   `ui_loop.cpp` on Core 1. Guarantees no mutual blocking.
2. **Shared state as single source:** all inter-core variables in
   `app_state.hpp/cpp`. Volatile semantics, no mutex.
3. **State machines isolated:** BAUD-DET, STOP-DET, auto-INV,
   auto-recovery — each with its own phases and local state, all
   inside `dsp_pipeline.cpp`.
4. **UI separate from logic:** `UIManager.hpp` is pure rendering,
   takes parameters by argument.

## 0a. Performance optimization (Build 189-194) — DONE

### Reducing Core 0 load (DSP)

**Before optimization (Build 188):** Core 0 = ~30 %, Core 1 = ~70 %.
**After optimization (Build 191+):** Core 0 = ~7 %, Core 1 = ~25-35 %.

Key optimizations:

1. **Strict Float Policy (Build 189):**
   - Full audit: all `double` → `float`, `sin()` → `sinf()`,
     `log10()` → `log10f()`
   - RP2350 has single-precision FPU; double-precision is software
     emulated (~10× slower)
   - Effect: Core 0 from ~30 % to ~15 %

2. **Compiler flags (Build 189):**
   ```cmake
   -O3 -ffast-math -funroll-loops
   -mfloat-abi=hard -mfpu=fpv5-sp-d16
   ```
   `-flto` **not** used — incompatible with Pico SDK `__wrap_`
   symbols.

3. **Hardware ADC FIFO (Build 190):**
   - `adc_fifo_setup()` + `adc_run(true)` for jitter-free 10 kHz
   - `tight_loop_contents()` instead of `__wfe()` (WFE loses samples
     without ADC IRQ)

4. **fast_log2f() (Build 190):**
   - IEEE 754 bit trick for the logarithm
   - ~4× faster than stock `log10f()`
   - Used in dB calculation for signal and SNR

5. **AGC precompute (Build 190):**
   - `1.0f / release` computed once → multiplication instead of
     division in the inner loop

6. **FFT on Core 1 (Build 191):**
   - FFT moved from Core 0 to Core 1 — only needed for spectrum
     rendering and SEARCH
   - Core 0 freed from the 1024-point FFT (~2 ms per frame)
   - Effect: Core 0 from ~15 % to ~7 %

7. **Ping-Pong DMA buffers (Build 190):**
   - Double buffering for SPI display
   - One strip drawn while the other is transferred

### Reducing Core 1 load (UI)

1. **Sprite rendering (LovyanGFX):** redraw only when data changes.
2. **Waterfall optimization:** direct SPI DMA for waterfall strips.
3. **FFT rate limiting:** every ~48 ms = 480 samples.
4. **Waterfall LUT + circular history buffer** (Build 219) — 480×64
   uint8 instead of 61 KB sprite, Core 1 lower bound 60 % → 39 %.

### Current load (Build 265)

- **Core 0:** 5-8 % (DSP idle) / 10-15 % (BAUD-DET active) / +1-2 %
  when NN gate is open
- **Core 1:** 25-35 % (depends on display mode)

## 1. Font system (Roadmap item #1)

### Stage 1: 4 font modes — DONE (Build 195)

- [x] BIG: Spleen 8×16 (9 rows, 55 chars)
- [x] MED: Bitocra 7×13 (11 rows, 62 chars)
- [x] SMALL: Font0 6×8 (15 rows, 73 chars)
- [x] TINY: Spleen 5×8 (17 rows, 90 chars) — Build 199
- [x] Converter `tools/bdf2gfx.py`
- [x] Automatic line_width adjustment when changing font
- [x] Save in flash

### Stage 2: Font Lab — TODO

A separate screen for fine font tuning (size, spacing, line_height).

### Stage 3: Skins and color schemes — TODO

- Classic Green (current)
- SDR Warm (dark blue background, warm waterfall palette)

## 2. Intelligent reception automation — DONE

### Auto mark/space inversion — DONE (Build 196-202)

- [x] Comparative algorithm (ERR before/after flip, ±3 % threshold)
- [x] NOR?/INV? indicator on uncertainty
- [x] SEARCH resets INV → NOR

### SEARCH (auto signal find) — DONE (Build 198-216)

- [x] FFT-based, all 8 shifts, multi-signal (up to 8)
- [x] Parabolic peak interpolation (Build 216)
- [x] Shift-proportional dedup tolerance (Build 216)
- [x] dist_penalty = 2.5 (Build 216)
- [x] Cycling (< 10 s between taps)

### Auto-shift detection — DONE (Build 200-203)

- [x] 8 standard shifts, SHIFT AUTO mode (idx=8)
- [x] Popup 3×3

### BAUD-DET (auto baud rate) — DONE (Build 206)

- [x] Symbol Duration Histogram + Harmonic Scoring
- [x] Fallback: ERR verify (sequential test)
- [x] 4 baud rates: 45.45 / 50 / 75 / 100
- [x] Popup 3×2

### STOP-DET (auto stop bit) — DONE (Build 205-218)

- [x] Direct gap measurement (state-7-end → next start-bit)
- [x] Warmup 1.5 s, idle filter 1.25T, bin boundaries 0.25/0.85T
  (Build 218)
- [x] Chain BAUD→STOP via shared_chain_stop_after_baud (Build 217)
- [x] Popup 2×2

### Full pipeline — DONE (Build 217)

- [x] SEARCH → SHIFT → BAUD (chain) → STOP → INV
- [x] Automatic chain, STOP waits for BAUD to finish
- [ ] Final screen "Found: 50 Baud, 450 Hz shift, 1.5 stop"

### Auto-Recovery — DONE (Build 217)

- [x] ERR > 15 % for 3 s → BAUD-DET → chain → STOP-DET
- [x] Protection against conflict with auto-INV

### Clipping Indicator — DONE (Build 216)

- [x] SIG bar blinks red/white on ADC clipping
- [x] "CLIP!" text blinks blue
- [x] 1.5 s latch

## 3. Hardware-accelerated rendering

- [ ] Hardware Scroll (ILI9488 VSCRSADD)
- [ ] SIO INTERP Colormap
- [x] Ping-Pong DMA Buffers (Build 190)
- [x] PIO-driven SPI at 60 MHz (Build 191+) — replaced software SPI
  with a PIO state machine

## 4. RP2350 optimization

- [x] Strict Float Policy (Build 189)
- [x] Hardware ADC FIFO (Build 190)
- [x] fast_log2f() IEEE 754 bit trick (Build 190)
- [x] AGC precompute (Build 190)
- [x] FFT on Core 1 (Build 191)
- [ ] Memory Barriers (`__dmb()`)
- [ ] CMSIS-DSP (arm_fir_f32, arm_biquad_f32)

## 5. UI optimization

- [ ] Selective Redraw
- [ ] Widget Framework
- [x] Eye Diagram with phosphor persistence (Build 194)
- [x] Error Rate Indicator, 3 thin bars (Build 191)
- [x] Tuning Lab with live ALPHA/K/SQ tuning (Build 194)
- [x] Inline NOTCH/VIT toggles in menu (Build 263, dropped the popup)
- [x] Red `*` for [ERR] on screen (Build 263)

## 6. Compiler flags

- [x] `-O3`, `-ffast-math`, `-funroll-loops` (Build 189)
- [x] `-mfloat-abi=hard`, `-mfpu=fpv5-sp-d16` (Build 189)
- **Note:** `-flto` is incompatible with Pico SDK `__wrap_`

## 7. Serial command interface

- [x] 40+ commands (Build 194-265)
- [x] Diagnostic stream `[D]` (Build 194)
- [x] `serial_cmd.ps1` with try/finally/Dispose + DTR/RTS (Build 217)
- [x] **B265 DUMP FRAMES** — per-frame soft-bit dump for NN training
  capture

## 8. Hybrid RTTY decoder — **GOAL ACHIEVED** (v2.0.0)

**Strategic goal:** decoding threshold **−15..−16 dB SNR** — better
than any existing RTTY decoder in the world.

**What I got versus the original plan:**

Architecture is different from the original Goertzel + Character-ML
plan. I went with **dual-IQ + LLR fusion + TinyML NN classifier**,
and it worked. So the substage statuses below reflect the actual path,
not the original.

### Architecture (actual, Phase 9)

```
                              ┌─ Path A (narrow FIR + I/Q + DPLL) ─┐
ADC → AGC → BPF → LMS notch ─┤                                     ├─ LLR fusion ─→ NN gate ─→ Baudot
                              └─ Path B (wide FIR + I/Q + DPLL) ────┘    (B264)
                                                                          │
                                                                          ▼
                                                                 Soft-Viterbi
                                                                 frame gate
```

### Actual results (multi-run AWGN, 3 seeds × 30 s dwell)

| Decoder | Claimed threshold | Real CER at −16 dB SNR |
|---|---:|---:|
| fldigi | ~−5 dB | (not head-to-head benched) |
| MMTTY | ~−9 dB | (not head-to-head benched) |
| 2Tone (current best) | ~−13 dB | **~58 pp real errors** |
| **TouchRTTY v2.0.0** | ~**−16 dB** | **~9 pp real errors** ✓ |

See `RELEASE_v2.0.0.md`.

### Stage 1: Dual-Goertzel matched filter — **N/A** (architecture changed)

Originally planned: a Goertzel filter parallel to I/Q. Instead I
implemented **dual-IQ architecture** — two parallel FIR+I/Q+DPLL
pipelines (narrow + wide) merging via LLR. Goertzel wasn't needed —
two I/Q chains cover the same case (narrow band for clean, wide for
drift) more cleanly and without separate synchronization.

### Stage 2: Multi-phase Goertzel — **N/A** (architecture changed)

See above — DPLL+PI controller on both chains closed the
multi-phase-sync need.

### Stage 3: Character-level ML — **DONE (v13 NN)** ✅

Achieved through PyTorch-trained MLP instead of 2Tone-style matched
filter:

- [x] **7→128→64→32 TinyML MLP** (~44 KB float32 weights)
- [x] **B264 confidence gate** — NN runs only when
  `data_min/sig_level < 0.20`
- [x] **PyTorch trainer** with per-sample loss weighting (v13
  production recipe)
- [x] **Soft output** — `nn_margin = top_logit − second_top_logit`
  used as a confidence measure
- [x] **Multi-run validation** — σ < 4 pp at key SNRs
- [x] **Reproducible**: code, weights, data, bench evidence all in repo

### Stage 4: Improvements beyond 2Tone

#### 4a. Contextual language prior (n-gram) — TRIED, NOT SHIPPED

Experiment in `tools/ngram_lm/` (see tree history). Gain of +1.63 pp
on the internal-corpus bench (B259), but didn't reproduce on real-air
consistently. Shelved.

#### 4b. FIGS/LTRS Viterbi — **DONE (B262 VIT)** ✅

Implemented as part of the **Soft-Viterbi frame validation gate**:

- [x] State machine framing with energy + parity validation
- [x] Configurable via the `VIT ON/OFF` serial command
- [x] Default ON in production

#### 4c. Adaptive Noise Blanker + Spectral Subtraction — PARTIAL ✅

- [x] **LMS adaptive notch chain** (`NOTCH ON/OFF`, Build 244+) —
  kills narrow carriers / heterodynes
- [ ] Impulse noise blanker (`> 3σ over 100 ms → mute 5 ms`) — TODO
- [ ] **Wiener Spectral subtraction** — TRIED, NOT SHIPPED. B258
  experiment (`NR ON/OFF`) gave neutral-to-harmful results under
  3-run honest averaging. Default OFF, code kept around in case I
  find the right threshold.

#### 4d. Temporal Diversity — N/A

Not implemented. Soft-Viterbi gate partially covers the case via
energy averaging.

#### 4e. Multi-band Goertzel for SEARCH — N/A

SEARCH remained FFT-based (multi-shift), fast enough.

#### 4f. Tiny Neural Net — **DONE (v13 production)** ✅

This is the **main win of this version.** v13 NN weighs ~44 KB, uses
the PyTorch sample_weight recipe. See Stage 3 above and
`docs/NN_TRAINING.md`.

Bonus: **B265 DUMP FRAMES** lets the user gather real-air training
data to retrain under their own conditions.

#### 4g. Soft Confidence UI — PARTIAL ✅

- [x] Red `*` for invalid frames on screen (B263)
- [x] Top bar shows NN/NOTCH/VIT status
- [ ] Color gradient of confidence (green/yellow/red) per character —
  TODO
- [ ] `[ML:94%]` in top bar — TODO

### Final CPU budget (Core 0 @ 300 MHz, B265)

| Component | CPU |
|---|---|
| ADC/AGC/FIR | ~2 % |
| Dual-IQ (A+B) + DPLL | ~3 % |
| LMS notch chain | ~0.5 % |
| Soft-Viterbi frame gate | ~0.5 % |
| NN inference (gate open) | +1-2 % (sparse) |
| BAUD-DET (when running) | +5-7 % (transient) |
| **Steady-state total** | **~7-10 %** |

Reserve ~90 % of Core 0 for future modes (CW, FT8, DRM).

## 9. Planned features

### SITOR-B / NAVTEX FEC — TODO (priority)

- [ ] Framer: 7 data bits + 1 stop
- [ ] CCIR 476 lookup (35 valid codewords, ratio 4:3)
- [ ] Time diversity buffer (5 symbols)
- [ ] Phasing sync (DX/RX signals)
- [ ] Auto-detect: 100/170 → try SITOR-B

### NN training: real-air oracle pipeline — TODO

The topic that remained after v2.0.0. Today real-air augmentation is
limited because labels come from hard-decision — the model can't
learn to beat hard-decision on uncertain frames, because for those
frames the labels are unknown. The solution:

- [ ] **DWD template matcher** — a parser for DWD weather format
  (predictable day-of-week, PPZ/QWZ patterns, wind directions) gives
  ground truth for known recordings
- [ ] Replay through HW in DUMP FRAMES mode, label uncertain frames
  against the oracle
- [ ] v14 NN training with **correct** real-air labels on uncertain
  frames — the path to push the −16 dB threshold by another 5-10 pp

### Built-in autotune — TODO

- [ ] AUTO button in Tuning Lab
- [ ] Hill-climb: ALPHA → BW → SQ
- [ ] Score = -5×ERR + SNR − 1000×|FE| + SQ_bonus

### Decoder-quality experiments — TODO

A backlog of DSP-side ideas I want to try, ranked by gain-per-effort.
Each one needs the multi-seed bench methodology (§ honest 3-run
averaging) to confirm it actually helps before going into production.

**Priority A — high gain, focused effort:**

- [ ] **Symbol-level MLSE against the 32-matrix Baudot alphabet.**
  Instead of bit-by-bit hard decisions, accumulate soft-bit energy
  into a 7-element vector for the whole character (165 ms for 45.45
  baud, 150 ms for 50 baud, including Start+Stop). At the end of the
  symbol, Euclidean-correlate against all 32 ITA-2 alphabet matrices.
  The maximum wins. This can "fill in" a character even if 2–3 of the
  5 data bits are destroyed. Cost: 32 × 7 multiplies per symbol —
  minimal, ~15 kHz macs at 50 baud. Expected: +1..2 dB vs current
  Soft-Viterbi framer. Requires framer refactor.

- [ ] **Soft-Viterbi start/stop energy validation.** The current
  Soft-Viterbi gate (B247) checks frame *structure*, not the actual
  energies of the Start and Stop bits. Adding energy checks on the
  1st and 7.5th bit slot would reject "character spray" frames in
  deep fades. Small patch, fully compatible with the existing Stage
  1.2 framer.

- [ ] **Gardner Early-Late Gate clock recovery.** Replace the
  zero-crossing DPLL with a Gardner gate: measure signal energy at
  the 25 % and 75 % bit slots, slow the clock if early-energy is
  higher, speed up if late, with a 0.05 phase-step clamp to avoid
  "phase bounces" in noise. Gardner works on peaks, not transitions,
  so it stays stable at low CNR where zero-crossings get unreliable.
  Big DPLL refactor but the most natural path to wringing more dB
  out of timing recovery.

**Priority B — easy wins, protection against erratic behavior:**

- [ ] **Flywheel DPLL confidence weighting.** Multiply the
  zero-crossing phase error by the current envelope weight. In a
  deep fade the DPLL "coasts" on its last stable frequency instead
  of getting yanked by noise. Drastically reduces jitter and
  character loss during fading. Small patch.

- [ ] **Semantic auto-inversion lockout.** A "semantic confidence"
  counter that increments on every ITA-2 control character (LTRS /
  FIGS). High confidence forbids the auto-INV flipper from running,
  so in deep fades the polarity doesn't oscillate between NOR and
  INV chasing noise.

- [ ] **Per-symbol Maximum Ratio Combining (MRC) in dual-IQ
  fusion.** The current Stage 3.3 LLR fusion uses a global SNR
  pre-estimate to weight the two IQ paths. A per-symbol version
  using dynamic `atc_mark_env` and `atc_space_env` inside the
  Euclidean distance is finer-grained and would prevent noise
  amplification when one tone fades out entirely (selective fading).

- [ ] **RTTY-aware LMS notch with "force field".** The current LMS
  notch chain is global. Adding a passband-protection rule (forbid
  the notch from entering Mark/Space ±margin) would let it hunt QRM
  more aggressively without ever killing the wanted signal itself.

- [ ] **Lookahead noise blanker.** Detect spikes in real time but
  mute the *delayed* audio (16 samples / ~1.6 ms later) — effectively
  "time-travel" and erase the rising edge of a lightning crack
  before the decoder ever sees it.

**Priority C — quality-of-life and squelch/AFC polish:**

- [ ] **25-percentile noise-floor histogram.** A 64-bin histogram
  for noise floor estimation instead of a simple mean. A strong RTTY
  signal doesn't lift the perceived noise floor, so squelch and SNR
  stay accurate in a congested band.

- [ ] **Parabolic AFC sub-bin interpolation.** 3 bins around the
  peak → parabolic vertex → 0.5 Hz tracking precision instead of
  choppy 10 Hz. Kills jitter on the on-screen markers.

- [ ] **Adaptive DPLL phase clamp.** ±0.3 wide for the first 5
  symbols after squelch opens (fast acquisition), then narrow to
  ±0.1 (jitter-resistant tracking).

- [ ] **Hot-loop caching of `expf()` constants.** Precompute
  `expf(time-constant)` once when baud changes instead of inside the
  10 kHz hot loop. Saves ~60 cycles per sample.

### Other

- [ ] Final screen "Found: ..." after SEARCH
- [ ] Color gradient of ML confidence on the text
- [ ] Multi-platform (ILI9341 320×240)
- [ ] SD card (DWD SYNOP parser)
- [ ] CW decoder (K-Means)
- [ ] I2S DAC audio output
- [ ] FT8 / FT4 mode
- [ ] WEFAX (HF weather fax)
- [ ] IQ-direct input from external SDR front-end (e.g. Belka-DX),
  dual-ADC capture, bypassing the audio path and AGC clipping. Worth
  +2-4 dB in marginal conditions; prerequisite for DRM (§9 of the
  archived Phase 8 plan).

---

*Current status: **v2.0.0 released**, branch `feat/alex-cl-dev`,
firmware build B265, NN weights v13.*

*v2.0.0 closed the strategic goal of section 8 — beat 2Tone at low
SNR. Further work on the NN side is described in
`docs/NN_TRAINING.md` (negative-result ledger + real-air oracle
direction).*
