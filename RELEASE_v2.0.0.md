# TouchRTTY v2.0.0 — Phase 9 + TinyML NN

> 🇷🇺 [Читать на русском](RELEASE_v2.0.0.ru.md)

**Release date:** 2026-05-12
**Firmware build:** B265
**NN weights:** v13 (`weight_uncertain=3.0` PyTorch recipe)
**Artifact:** [`TouchRTTY_v2.0.0.uf2`](TouchRTTY_v2.0.0.uf2)
**Previous release:** [v1.72](https://github.com/Alex-Electron/TouchRTTY/releases/tag/v1.72) (March 2026)

---

## Headline

TouchRTTY v2.0.0 is a substantial rewrite over v1.72 — a fundamentally
new decoder architecture with a learned post-classifier on top. On the
same audio where [2Tone 26.01a](https://www.rttycontesting.com/downloads/2tone/)
collapses into random letters at low SNR, this build produces readable
telegraphy. I benchmarked it against 2Tone over multiple seeds, and the
multi-run-averaged numbers are:

| SNR | TouchRTTY NN OFF | TouchRTTY NN ON | 2Tone (real errors) |
|---:|---:|---:|---:|
| −12 | 16 % | **15 %** | ~22 pp |
| −14 | 32 % | **23 %** (σ = 1.5) | — |
| −16 | 78 % | **55 %** (σ = 3.2) | ~58 pp |
| −20 | 88 % | **80 %** (σ = 2.3) | — |

That's 3–6× lower real error rate at the SNRs where it matters. The low
standard deviation matters more than the raw numbers — it means the
improvement is reproducible across seeds, not a lucky run. The reference
2Tone bench (committed evidence, the same audio that produced the
gibberish vs telegraphy comparison) is reproducible on your own loopback — see
[`docs/BENCH_TOOLING.md`](docs/BENCH_TOOLING.md) for the procedure.

---

## What's new vs v1.72

### A whole new decoder architecture (Phase 9)

* **Dual-IQ paths** — narrow A and wide B filter chains running in
  parallel
* **LLR fusion** — log-likelihood-ratio combination of A and B, with
  optional SNR-weighted dynamic mode (`DYN ON`)
* **Soft-Viterbi frame validation** — full energy + parity gate for
  Baudot frames; tunable via `VIT ON/OFF`
* **LMS adaptive notch chain** — `NOTCH ON/OFF` toggle kills narrow
  carriers inside the audio passband
* **DPLL with PI controller** — `ALPHA` live-tunable from screen or
  serial
* **AFC** — ±100 Hz drift tracking from the configured `FREQ`
* **AGC** — fast attack, slow release
* **SNR-based squelch** with hysteresis

### Neural-net post-classifier

A small (7→128→64→32, about 44 KB float32) MLP gets a vote on Baudot
frames where the soft-bit pattern is uncertain.

* **B264 confidence gate** — the NN runs only when the weakest data bit
  drops below 20 % of estimated signal level. Above that the hard
  decision is trusted untouched. This is what eliminates the pre-gate
  U-shape where NN helped at threshold but hurt at comfortable SNR.
* **Production weights are v13** — PyTorch trainer, key trick is
  per-sample loss weighting (3× boost for `data_min < 0.30` frames).
  sklearn `MLPClassifier` doesn't support `sample_weight`; that's the
  actual reason for the torch port.
* **DUMP FRAMES serial command** — emits per-frame soft bits + hard
  decision labels, suitable for capturing real-air training data and
  augmenting the synthetic set.

### A real UI

* **3-bar top panel** — SIG / AGC / ERR (rolling 100-frame window)
* **Tuning Lab** with phosphor-persistent eye diagram and 6×2 button
  grid for ALPHA / K (LPF bandwidth) / squelch
* **Menu overlay** with PATH cycle (A / B / HYB / HYB+NN), NOTCH / VIT
  toggles, DISP mode cycle (waterfall / spectrum / scope)
* **DIAG screen** with character histogram and zero-bias meter
* **Touch calibration** with 4-corner overlay
* **Factory-reset dialog** with explicit YES/NO confirmation
* **Red `*` for [ERR]** on screen (full token preserved on serial)
* **PIO-driven ILI9488** at 60 MHz with DMA waterfall — ~20 FPS

### A real serial command system

What was rudimentary in v1.72 is now a complete CLI:

* Live tuning: `ALPHA`, `BW`, `SQ`, `FREQ`
* Protocol: `BAUD`, `SHIFT`, `STOP`, `INV` (each accepts AUTO)
* Toggles: `AFC`, `AGC`, `SCALE`, `NN`, `NOTCH`, `VIT`, `NR`
* Decoder path: `PATH A / B / HYB`, `DYN ON/OFF`, `WEIGHTS`
* Persistence: `SAVE`, `CLEAR`, `STATUS`, `VERSION`
* Diagnostics: `DIAG ON/OFF`, `DUMP SPEC`, `DUMP MS`, `DUMP FRAMES`
* Help: `HELP`, `SEARCH`

Full reference: [`docs/SERIAL_COMMANDS.md`](docs/SERIAL_COMMANDS.md).

### Reproducible bench and training tooling

`tools/` ships a complete Python suite for:

* Generating synthetic RTTY WAVs with controlled SNR (`rtty_gen.py`,
  `sweep_runner.py`, browser-side `rtty_simulator.html`)
* Playing recorded audio through hardware and capturing serial decode
  (`bench_replay.py`)
* AWGN sweep benchmarks with NN-OFF vs NN-ON comparison
  (`nn_sweep_compare.py`)
* Multi-seed averaging (`aggregate_compare.py`) — single-run benches at
  low SNR are noisy enough that production decisions need ≥ 3 seeds
* CER analysis (`cer_analyze.py`)
* PyTorch NN training (`train_nn_torch.py`) with the v13 production
  recipe one flag away
* B265 `DUMP FRAMES` log → numpy npz parsing (`parse_dump_frames.py`)
* Overnight chain runner (`overnight_runner.sh`) for unattended
  evaluation of multiple recipes

Full decision tree: [`docs/BENCH_TOOLING.md`](docs/BENCH_TOOLING.md).

### Documentation

Six long-form docs in `docs/`:

* [`HARDWARE_SETUP.md`](docs/HARDWARE_SETUP.md) — GPIO pinout, bias
  network, build/flash, troubleshooting
* [`SERIAL_COMMANDS.md`](docs/SERIAL_COMMANDS.md) — full CLI reference
  with examples
* [`MENU_GUIDE.md`](docs/MENU_GUIDE.md) — touchscreen UI walkthrough
* [`NN_TRAINING.md`](docs/NN_TRAINING.md) — production v13 recipe + the
  negative-result ledger explaining what I tried and what didn't work
* [`BENCH_TOOLING.md`](docs/BENCH_TOOLING.md) — bench scripts decision
  tree and workflows
* Plus historical Phase 9 design docs and lessons-learned notes

---

## Breaking changes

* **RP2040 is no longer supported.** This release requires the **Raspberry
  Pi Pico 2 (RP2350)**. The firmware doesn't fit in the RP2040's SRAM
  and the DSP needs the M33's FPU. If you're running v1.72 on an
  RP2040, do not flash this release.
* The serial protocol is mostly additive (old commands still work) but
  some defaults changed (`PATH HYB+NN` is now the recommended setting;
  was just `A` before).
* Some early-phase planning documents (`docs/PHASE1..PHASE7_*.md`) were
  removed from `docs/`. They live on in git history and were superseded
  by the Phase 9 implementation.

---

## How to flash

```bash
picotool load -f TouchRTTY_v2.0.0.uf2
```

Or hold BOOTSEL while plugging the Pico in, then drag-and-drop the
`.uf2` onto the `RPI-RP2` mass-storage drive.

After flashing, send `VERSION` over serial to confirm:

```
>> TouchRTTY Phase9 B265 (built May 12 2026 ...)
```

---

## Building from source

```bash
git clone --recurse-submodules https://github.com/Alex-Electron/TouchRTTY.git
cd TouchRTTY
mkdir build && cd build
cmake -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk ..
ninja
picotool load -f TouchRTTY.uf2
```

Requires Pico SDK 2.x and an ARM toolchain.

---

## Roadmap

There's plenty of headroom left in the NN — the negative-result ledger
in [`docs/NN_TRAINING.md`](docs/NN_TRAINING.md) documents 10+ recipes I
tried that didn't pay off, and the most promising remaining direction
is building a trusted-oracle pipeline for labelling uncertain real-air
frames (DWD template matching). Real-air augmentation today is limited
because hard-decision labels can't teach the NN to beat hard decision.

Beyond NN, the historical roadmap (SD card, CW, FT8/FT4, WEFAX) is
sketched in [`docs/ROADMAP_OPTIMIZATION.md`](docs/ROADMAP_OPTIMIZATION.md).

---

## Credits and acknowledgements

* Pico SDK © Raspberry Pi (BSD-3-Clause)
* [LovyanGFX](https://github.com/lovyan03/LovyanGFX) © lovyan03 (FreeBSD)
* G3YYD's 2Tone — referenced as a benchmark only, not redistributed
* The DWD weather service for being a reliable 24/7 source of test signal
