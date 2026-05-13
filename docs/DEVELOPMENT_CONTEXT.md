# TouchRTTY — full development context

> 🇷🇺 [Читать на русском](DEVELOPMENT_CONTEXT.ru.md)

*Updated: 2026-05-12 — **Build B265 / v2.0.0** (Phase 9 + TinyML NN).*

This is the single snapshot of where the project stands, written for a
developer walking in cold. It doesn't duplicate release notes; it ties
the architecture, the development phases, and the docs together so you
know where to look next.

---

## 0. TL;DR

- **Goal of the project:** a standalone RTTY decoder on RP2350 + ILI9488
  that decodes better than 2Tone, fldigi, and MMTTY at low SNR.
- **Goal achieved in v2.0.0** (2026-05-12). Multi-run AWGN bench: at
  −16 dB SNR TouchRTTY hits ~9 pp real CER versus 2Tone 26.01a's
  ~58 pp. Details — `RELEASE_v2.0.0.md`.
- **Current branch:** `feat/alex-cl-dev` (pushed to GitHub as a single
  squashed `v2.0.0` commit). `main` is still behind.
- **What's next:** real-air NN oracle pipeline, SITOR-B/NAVTEX FEC,
  Phase 10 (see `NEIGHBOR_IDEAS.md`). Details in section 6.

**Repo:** `https://github.com/Alex-Electron/TouchRTTY.git`
**Path:** `C:\Temp\TouchRTTY`

---

## 1. Development phases (timeline and status)

The phases did not strictly run in order — priorities shifted with
bench results. The canonical order:

| Phase | Window | Status | Where to look |
|---|---|---|---|
| **Phase 1** — Architecture / module split | up to B189 | ✅ DONE (v1.x) | `docs/archive/phase1-8/PHASE1_ARCHITECTURE.md` |
| **Phase 2** — UI / state machines (top bar, popups) | B190–B210 | ✅ DONE (v1.x) | `docs/archive/phase1-8/PHASE2_UI_STATE.md` |
| **Phase 3** — RTTY DSP (Mark/Space IQ, DPLL, Baudot, BAUD-DET, STOP-DET, SEARCH) | B210–B240 | ✅ DONE (v1.72) | `docs/archive/phase1-8/PHASE3_RTTY_DSP_FINAL.md` |
| **Phase 4** — SD card (logging, DWD SYNOP parser) | — | ⏸ PLANNED | `docs/archive/phase1-8/PHASE4_SD_CARD_PLAN.md` |
| **Phase 5** — CW advanced decoder | — | ⏸ PLANNED | `docs/archive/phase1-8/PHASE5_CW_ADVANCED_DECODER.md` |
| **Phase 6** — FT8 / FT4 | — | ⏸ PLANNED | `docs/archive/phase1-8/PHASE6_FT8_FT4_PLAN.md` |
| **Phase 7** — WEFAX (HF weather fax) | — | ⏸ PLANNED | `docs/archive/phase1-8/PHASE7_WEFAX_PLAN.md` |
| **Phase 8** — DRM | — | ⏸ PLANNED | `docs/archive/phase1-8/PHASE8_DRM_PLAN.md` |
| **Phase 9** — Hybrid RTTY decoder (dual-IQ + LLR + Soft-Viterbi + TinyML NN) | B242–B265 | ✅ DONE (v2.0.0) | `RELEASE_v2.0.0.md`, `docs/PHASE9_*.md` |
| **Phase 10** — research backlog (Symbol-MLSE, Gardner, n-gram LM, IQ-вход) | — | 🔬 RESEARCH | `docs/NEIGHBOR_IDEAS.md` |

> **Why Phase 9 ran before Phase 4-8.** Priority shifted to the
> strategic goal "beat 2Tone at low SNR". Phase 4-8 are *mode
> expansion* (new signal types), while Phase 9 is *decoder quality*
> for RTTY — which is central to the device. By the time I'd
> accumulated enough knowledge (see archived Phase 3 + neighbor
> ideas), Phase 9 was the obvious next step.

Phase 1-8 archives live in `docs/archive/phase1-8/` — historical design
docs that drove the v1.x implementation. Still useful as the
pre-Phase-9 reference. Phase 4-8 describe **plans** that haven't been
implemented yet.

---

## 2. Current architecture (v2.0.0)

### 2.1 Hardware

- **MCU:** RP2350 (dual Cortex-M33, FPU, 150 MHz)
- **Display:** ILI9488 480×320 TFT, PIO SPI @ 60 MHz, XPT2046 touch
- **Audio in:** ADC0 (GPIO26), 10 kHz sample rate, 1.65 V bias
- **Build:** CMake + Ninja + Pico SDK 2.x, flashed via picotool

Pinout and bias-network schematic — `docs/HARDWARE_SETUP.md`.

### 2.2 Dual-core architecture

- **Core 0 (DSP, ~7-10%)** — `dsp_pipeline.cpp`:
  10 kHz hard-real-time loop. ADC DMA → AGC → BPF 300-3000 Hz →
  LMS notch chain → dual-IQ path A (narrow) + path B (wide) →
  LLR fusion (HYB) → DPLL/PI → bit slicing → Soft-Viterbi frame gate →
  B264 confidence gate → optional NN inference → Baudot → ITA-2 ASCII.
- **Core 1 (UI, ~25-35%)** — `ui_loop.cpp`, `ui/UIManager.hpp`:
  FFT (1024-point) → waterfall/spectrum/scope → SEARCH → touch
  handling → serial command parser → display render.

Inter-core data goes through `volatile` shared variables in
`app_state.hpp/.cpp` (lock-free, no mutex). Full list — see the source.

### 2.3 Signal flow

<p align="center">
  <img src="images/signal_flow.png" alt="TouchRTTY signal flow" width="520">
</p>

See also `README.md` (simplified block diagram) and
`docs/PHASE9_HYBRID_DECODER_PLAN.md` (detailed design).

---

## 3. Subsystems and where to find them

| Subsystem | File | Doc |
|---|---|---|
| Entry point, init | `src/main.cpp` | — |
| DSP pipeline (Core 0) | `src/dsp_pipeline.cpp` | `docs/PHASE9_HYBRID_DECODER_PLAN.md`, archived Phase 3 |
| UI loop (Core 1) | `src/ui_loop.cpp` | `docs/MENU_GUIDE.md` |
| Serial CLI (40+ commands) | `src/serial_commands.cpp` | `docs/SERIAL_COMMANDS.md` |
| Shared state | `src/app_state.{hpp,cpp}` | — |
| Flash settings | `src/settings_flash.cpp` | `docs/MENU_GUIDE.md` (SAVE) |
| Touch calibration | `src/touch_xpt2046.cpp` | `docs/HARDWARE_SETUP.md` |
| Display driver | `src/display/ili9488_driver.h`, PIO | — |
| UI render | `src/ui/UIManager.hpp` | `docs/MENU_GUIDE.md` |
| NN inference | `src/dsp_pipeline.cpp` (B264 gate + MLP), `src/dsp/nn_weights.h` | `docs/NN_TRAINING.md` |
| Build number | `src/version.h` (current: **B265**) | — |

---

## 4. What changed in v2.0.0 vs v1.72

Full release notes — `RELEASE_v2.0.0.md`. The short version:

- **Phase 9 architecture** — dual-IQ paths + LLR fusion + Soft-Viterbi
  frame validation + LMS notch + DPLL PI + AFC ±100 Hz + SNR squelch.
- **TinyML NN** (v13 weights) — 7→128→64→32 MLP (~44 KB), only fires
  when the **B264 confidence gate** is open (`data_min < 0.20·sig`).
  PyTorch trainer with per-sample loss weighting
  (`weight_uncertain=3.0`).
- **DUMP FRAMES** — a serial command that streams per-frame soft-bits
  + hard decision for capturing training data.
- **UI** — Tuning Lab with persistent eye diagram, PATH cycle
  (A/B/HYB/HYB+NN), inline NOTCH/VIT toggles, red `*` for invalid
  frames, factory-reset dialog.
- **40+ serial CLI commands**: live tuning, persistence, diagnostics,
  NN control, dump frames/spectrum/mark-space.
- **Docs** — six long-form guides in `docs/` (Hardware, Serial, Menu,
  NN training, Bench, plus this file).

### Breaking changes

- **RP2040 is no longer supported** — RP2350 required (FPU + SRAM).
- Default `PATH HYB+NN` instead of `PATH A`.
- Early-phase planning docs `PHASE1..PHASE7_*.md` were moved out of
  `docs/` root to `docs/archive/phase1-8/`.

---

## 5. Development tooling

Full decision tree — `docs/BENCH_TOOLING.md`. Main scripts:

| Script | What it does |
|---|---|
| `tools/rtty_simulator.html` | In-browser RTTY signal generator |
| `tools/rtty_gen.py` | Synthesizes WAV with controlled SNR (AWGN) |
| `tools/sweep_runner.py` | SNR ladder via HW + serial capture |
| `tools/bench_replay.py` | Replay a recorded WAV → serial log |
| `tools/nn_sweep_compare.py` | NN-OFF vs NN-ON A/B sweep |
| `tools/aggregate_compare.py` | Multi-seed mean ± σ aggregation |
| `tools/cer_analyze.py` | CER analysis (cyclic-rotation aware) |
| `tools/train_nn_torch.py` | PyTorch NN trainer (v13 recipe) |
| `tools/parse_dump_frames.py` | B265 DUMP stream → numpy npz |
| `tools/overnight_runner.sh` | Train + sweep chain for unattended runs |
| `tools/send_serial_cmd.py` | One-shot serial command |

The internal-only scripts (overnight harnesses, training prep, etc.)
stay out of the public branch — what's published is what you need to
reproduce the release.

### Building and flashing

```bash
git clone --recurse-submodules https://github.com/Alex-Electron/TouchRTTY.git
cd TouchRTTY && mkdir build && cd build
cmake -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk ..
ninja
picotool load -f TouchRTTY.uf2
```

On Windows (this dev rig) I always flash via `picotool`, not
drag-and-drop into RPI-RP2 — see memory `feedback_picotool.md`.

---

## 6. What's next (post-v2.0.0)

Full backlog — `docs/ROADMAP_OPTIMIZATION.md` §9. Priorities:

1. **Real-air NN oracle pipeline** — a DWD template matcher gives
   ground truth for uncertain frames, expected to move the threshold
   from −16 to −20 dB. Right now real-air augmentation is limited
   because labels come from hard-decision (the NN can't learn to
   beat hard-decision).
2. **SITOR-B / NAVTEX FEC** — 100 baud / 170 Hz, CCIR 476, ratio 4:3,
   time diversity. See memory `project_sitorb.md`.
3. **Phase 10** — `docs/NEIGHBOR_IDEAS.md`: Symbol-MLSE, Gardner clock
   recovery, Flywheel DPLL, semantic auto-INV lockout. Each is its
   own experiment with multi-seed bench validation.
4. **Phase 4-8** — mode expansion (CW advanced, FT8/FT4, WEFAX, DRM,
   SD card). Lower priority — a separate strategic line.
5. **UI palettes / skins** — memory `project_ui_palettes.md` ("hacker
   green" and others). Cosmetic.

---

## 7. Known limitations

1. **Real-air NN improvement plateau** — without an oracle pipeline,
   the NN can only train on synthetic data plus hard-decision labels
   from real-air. See the negative-results ledger in
   `docs/NN_TRAINING.md`.
2. **425 vs 450 Hz shift** — FFT resolution ~10 Hz/bin, 2.5-bin gap is
   indistinguishable under FSK keying spectral smear. Workaround:
   manual SHIFT.
3. **Memory barriers** — no `__dmb()` between cores, theoretical race
   on shared volatile. Doesn't reproduce in practice. TODO.
4. **2Tone benchmark** — the N1MM emulator doesn't complete the
   handshake to 2Tone 26.01a, so DSP gets disabled. For head-to-head
   comparison I use audio loopback (Voicemeeter) + manual launch.
   Memory `project_2tone_unreliable.md`.

---

## 8. Pointers

| If you want to … | Open |
|---|---|
| Flash and start using it | `README.md` + `docs/HARDWARE_SETUP.md` |
| Understand the CLI | `docs/SERIAL_COMMANDS.md` |
| Understand the touchscreen UI | `docs/MENU_GUIDE.md` |
| Train your own NN | `docs/NN_TRAINING.md` |
| Run a bench | `docs/BENCH_TOOLING.md` |
| Full roadmap / DONE-list | `docs/ROADMAP_OPTIMIZATION.md` |
| What was in v2.0.0 | `RELEASE_v2.0.0.md` |
| Phase 9 design history | `docs/PHASE9_HYBRID_DECODER_PLAN.md` |
| Phase 9 progress snapshot B245 | `docs/PHASE9_PROGRESS_REPORT.md` |
| Phase 10 ideas | `docs/NEIGHBOR_IDEAS.md` |
| Phase 1-8 archive | `docs/archive/phase1-8/` |

---

*Living document. Next major architecture change — update the "phases"
section and this snapshot.*
