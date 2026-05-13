# Changelog: TouchRTTY (RP2350)

> 🇷🇺 [Читать на русском](CHANGELOG.ru.md)

All notable changes to this project will be documented in this file.

---

## [v2.0.0 — Phase 9 + TinyML NN] - 2026-05-12

Major rewrite over v1.72. Full release notes in
[`RELEASE_v2.0.0.md`](RELEASE_v2.0.0.md).

### Headline

* **Beats 2Tone 26.01a at low SNR** by 3-6× real error rate on the same
  audio. Multi-seed averaged AWGN bench over SNR -4..-22 dB.
* **Production NN weights (v13)** delivered with the firmware:
  PyTorch-trained MLP with `weight_uncertain=3.0` recipe. Improves
  -14/-16/-20 dB SNR by -1.9/-1.8/-9.1 pp vs prior production weights
  with σ < 4 pp across seeds.

### Added

* **Phase 9 hybrid decoder architecture** — dual-IQ paths (narrow A,
  wide B) fused via LLR (HYB), Soft-Viterbi frame validation gate,
  LMS adaptive notch chain, DPLL with PI controller, AFC/AGC.
* **TinyML NN classifier** (`NN ON/OFF`) — 7→128→64→32 MLP, ~44 KB
  weights, B264 confidence-gated to run only on uncertain frames.
* **B265 DUMP FRAMES** serial command — per-frame soft-bit + label
  stream for capturing real-air training data.
* **Tuning Lab** UI with phosphor-persistent eye diagram and live
  ALPHA / K / SQ adjustment.
* **3-bar top panel** showing SIG / AGC / ERR (rolling 100-frame
  error window).
* **Complete serial command system** — 40+ commands documented in
  `docs/SERIAL_COMMANDS.md`.
* **Reproducible bench tooling** — PyTorch trainer, AWGN sweep with
  NN-OFF vs NN-ON comparison, multi-seed aggregator, real-air
  bench. All documented in `docs/BENCH_TOOLING.md`.
* **Browser-side RTTY simulator** (`tools/rtty_simulator.html`) for
  signal generation without the Python stack.
* **Six long-form documentation files** in `docs/` covering hardware
  setup, serial commands, on-device menu, NN training, and bench
  tooling.

### Changed

* **Build counter** now at B265 (consolidates B194..B265 of incremental
  Phase 9 work).
* **PATH** UI menu cycles four states (`A / B / HYB / HYB+NN`) instead
  of three; `HYB+NN` is the recommended production setting.
* **`[ERR]` rendering on screen** collapsed to a single red `*` glyph
  (B263). Full `[ERR]` token preserved on serial.
* **NOTCH / VIT** moved from popup to inline menu toggles for
  fewer taps.

### Breaking

* Old per-phase planning docs (`PHASE1..PHASE7_*.md`) removed from
  `docs/` (superseded by Phase 9 implementation). Available in git
  history.

### Release artifact

`TouchRTTY_v2.0.0.uf2` — flashable via `picotool` or BOOTSEL drag-drop.

---

## [B258 — Stage 4 closed: Wiener NR neutral-to-harmful under honest 3-run averaging] - 2026-04-19

### Added
**`NR ON` / `NR OFF` serial commands** (`src/serial_commands.cpp`) plus
the `shared_spectral_nr` flag (`src/app_state.{hpp,cpp}`). Default
**OFF** — see below for the autopsy.

**Per-bin Wiener noise reduction** (`src/dsp_pipeline.cpp`, guarded by `shared_spectral_nr`):
- Asymmetric min-tracker for the floor of each of the 4 power streams
  (`mark_a`, `space_a`, `mark_b`, `space_b`): `fast-down 0.1`, `slow-up 1e-5`.
- Per-channel Wiener gain `G = (P − floor) / P` with floor-clamp `G ≥ 0.7`.
- SNR-gated via `shared_snr_db` from the Core 1 FFT (engages only when
  SNR is below the threshold — most likely the threshold was set wrong).

### Fixed
**Stage 4 doesn't work — honest measurement showed neutral-to-harmful.**
After the B253-B256 experiments (different floors, gating, LPF emulation)
and measurements via single sweep runs it looked like a ~5% improvement
at -14 dB. But **rerunning the same B256 gave up to 9% CER spread** on
the identical config → the baseline is unstable thanks to AWGN realization
and Windows audio jitter. Stage 4 closed:
- 3-run averaging (`tools/cer_avg.py`, see below) over B257:
  - `-14 dB`: NRON 22.58% mean vs NROFF **18.56%** mean → NRON **worse** by 4.02%
  - `-16 dB`: NRON 59.45% mean vs NROFF **48.05%** mean → NRON worse by 11.4%
  - `-18 dB`: NRON 80.37% mean vs NROFF 79.95% mean → ~zero
- Symmetric application of gain to both channels is mathematically a no-op
  (downstream AGC `atc_mark_env`/`atc_space_env` normalizes both in the
  same ratio). Asymmetric application breaks LLR invariants (per-channel
  SNR estimates become biased).
- Code left behind the `shared_spectral_nr` gate (OFF by default) — in
  case Stage 5 wants spectral subtraction as a component of a different
  architecture.

### Next
- Stage 5 variant 1 (matched filter / Path A LPF narrower): test `BW`
  command at `{0.4, 0.5, 0.6, 0.75, 0.9}` × 3 sweep runs each →
  `cer_avg.py` aggregation.
- Remaining Stage 5 candidates: BCJR soft-output, character-level LM,
  ML classifier on shift-register logs.

---

## [B257 — tools/cer_avg.py: N-run CER aggregation (mean/std/min/max)] - 2026-04-19

### Added
**`tools/cer_avg.py`** — runner over `cer_analyze.py` that averages CER
over N sweep+log pairs:
```
python cer_avg.py --gt "RYRYRY..." \
  --pairs run1.sweep:run1.log run2.sweep:run2.log run3.sweep:run3.log
```
Output: for each `(SNR, PATH)` tuple prints `N mean std min max`. Built
this to expose that single sweep runs are unstable to within ±9% CER —
without averaging it's **impossible** to tell a real decoder improvement
from the noise of an AWGN realization.

### Fixed
**`tools/sweep_runner.py` — post-noise rescale preserves SNR.**
Previously, on clipping (`peak > 0.95`) the rescale was applied only to
the audio without accounting for the fact that the noise inside
`add_awgn()` is computed relative to the signal rms **before** generation.
At large negative SNRs that led to overflow and loss of measurement
precision. New behaviour:
- Pre-scale the `clean` signal by `--sig-level` (default 0.10 = -20 dBFS)
  **before** add_awgn, so that at the minimum SNR in the ladder the
  noise fits within ±1.0 without clipping.
- If peak > 0.95 still happens after noise overlay — the rescale is
  applied to **both** (the signal is already inside the audio), SNR is
  preserved (same multiplier applied to everything), the decoder copes
  via AGC.

**`tools/cer_analyze.py` — `--lag` compensation for serial batching.**
Firmware flushes accumulated chars only when a newline arrives — and the
newline usually arrives on the next `[CMD] PATH=X` echo. So characters
decoded during bin N end up in the log under bin N+1's timestamp. The
`--lag 2.0` option (default) shifts record timestamps back by 2 seconds
before bin assignment → correct attribution of characters to SNR/PATH.
Without this the first window of a sweep systematically showed its CER
as belonging to the next window (first-window bias).

### Next
- Use `cer_avg.py --pairs` for at least 3 runs on every future
  measurement — no more trusting single-shot numbers.

---

## [B252 — Stage 3.3 TUNED: dynamic SNR-weighted LLR fusion] - 2026-04-19

### Added
**Stage 3.3 — dynamic LLR fusion** (`src/dsp_pipeline.cpp`, `shared_dyn_fusion`):
replaced the equal-weight geometric mean of the two IQ paths with weights
proportional to per-path SNR. Final formula after tuning:
```
w_a = sqrt(snr_a_ema) / (sqrt(snr_a_ema) + sqrt(snr_b_ema))
w_a = clamp(w_a, 0.2, 0.8)    // guard against single-path lock-in
α    = 0.002                   // per-path EMA (slow update)
mark  = exp(w_a·log(mark_a)  + w_b·log(mark_b))
space = exp(w_a·log(space_a) + w_b·log(space_b))
```
Where `snr_{a,b} = max(mark, space) / min(mark, space)` on every
processed sample.

**`DYN ON` / `DYN OFF` serial commands** plus `shared_snr_a_ema` /
`shared_snr_b_ema` telemetry (for weight-convergence diagnostics).

**`PATH LLR` alias** for `PATH HYB` (compatibility with external scripts
that may use the old name).

**`WEIGHTS <wa> <wb>` command** — static Stage 3.2 weight override for
A/B ablation (internally normalised to sum=1.0).

### Key measurement
3-run averaged threshold (CER ≥ 5%):
- **before Stage 3 (B230 baseline)**: ~ -11 dB
- **B252 Stage 3.3 TUNED HYB**: **~ -14 dB** → honest +3 dB gain vs pre-stage
- Smaller α (0.001) and a tighter clamp (0.3..0.7) were tested too;
  the winner was α=0.002 + sqrt-softening + a wide [0.2..0.8] clamp.

### Fixed
**Stage 3.2 weighted fusion bias** — a single B249 measurement with
fixed `WEIGHTS 0.7 0.3` had given an implausibly good -18 dB result.
The cause: first-window bias (the first sweep window contains the clean
signal before AWGN is mixed in). A reversed-order control run (B251 rev)
confirmed: the first window is always contaminated regardless of the
DYN ON/OFF order. All measurements now use `--trim 3.0` (skip the first
3 s of each window) + `--lag 2.0` (batching compensation) + cer_avg.py
over 3 runs.

### Next
- Stage 4 (Wiener spectral NR) — see B258 (failed, reverted).
- Stage 5 variant 1: matched filter tuning via a BW sweep.

---

## [B247 — serial VERSION command + cer_analyze diag-strip fix] - 2026-04-19

### Added
**`VERSION` / `VER` / `ID` commands** (`src/serial_commands.cpp`) —
prints `>> TouchRTTY Phase9 B<N> (built <DATE> <TIME>)` so automation
can verify which firmware is currently on the device. Discovered that
the RP2350 had been carrying firmware from a neighbouring project
(answering `UNKNOWN COMMAND: PATH A` and injecting
`[HYBRID DIVERGENCE: Legacy=… ML=…]` into the serial output), which is
why the B246 A/B/HYB sweep measurements were invalid.

### Fixed
**`tools/cer_analyze.py` — clean_decoded** now strips diagnostic lines
correctly:
- Regex `\[D\][^\n]*` → per-record strip (previously joined everything
  with spaces and stripped a single big string, killing everything after
  the first `[D]`).
- Added filters `\[HYBRID[^\]\n]*\]?[^\n]*`, `>>[^\n]*`, `===[^\n]*`.
- Join records via `''.join(clean_decoded(c) for c in chars)` (concat
  with no separator — the char-stream firmware emits with no space
  between characters).

The previous version was returning CER=90% even at +20 dB (the contents
of `[D] SNR=... ERR=...` were leaking into "decoded"); now at +20 dB we
see a real CER of 15-30% (the residual 15% is FIGS-table mismatch
between rtty_gen and firmware ITA2, worked around for now via
digit-free text).

### Next
- Sweep with `--text "RYRYRY THE QUICK BROWN FOX JUMPS OVER LAZY DOG "`
  (no digits/symbols, FIGS never triggers → pure bit-decision CER).
- Stage 3.2 weighted fusion only after a valid A/B/HYB baseline.

---

## [B246.1 — testbench: impulse default-off + audio sink selector] - 2026-04-18

### Fixed
**Critical bug in `tools/rtty_simulator.html` (introduced in B240.1)**:
the `Impulse noise (atmospherics / QRN)` checkbox had `checked` set by
default, with rate=120/min, duration=10 ms, amplitude=×10. As a result
**every sweep measurement B242 → B246/B** (baseline B230, Soft-LLR,
Soft-Viterbi, LMS-notch AWGN, Input BPF, Dual IQ path A/B) was run with
impulse noise overlaid, not clean AWGN. The decoder thresholds in those
tables are **more pessimistic** than real AWGN by an unknown amount.

- `impEnable` no longer has `checked` — impulses are now off by default.
- All B242→B246 measurements need to be re-shot in clean AWGN; the
  retrospective analysis of relative gain per stage transition stays
  valid (impulses affect every stage equally), but the absolute
  thresholds do not.

### Added
**Audio output selector** in the simulator (`Audio output` fieldset at
the top):
- Dropdown listing every output device via
  `navigator.mediaDevices.enumerateDevices()`.
- A "Show device names" button — requests
  `getUserMedia({audio:true})` for 1 ms (then stops immediately) so
  that Chrome/Firefox unblocks the real device names instead of showing
  `Output 1 (hash...)`.
- Auto-refresh on `devicechange` (USB sound card plug/unplug).

**Central output-bus (`masterBus`)**: all 5 sources (signal gain, AWGN
noise, CW QRM, RTTY QRM, impulse bursts) now converge on `masterBus`,
not on `audioCtx.destination`. Routing:
- Default → `masterBus → audioCtx.destination` (as before).
- Device chosen → `masterBus → MediaStreamDestinationNode →
  <audio>.setSinkId(deviceId)`.

Going through `HTMLMediaElement.setSinkId` (any recent Chrome,
Firefox 116+) — unlike the experimental `AudioContext.setSinkId`, which
is not supported in Firefox and frequently doesn't work in Chrome.

### Why
My laptop has multiple sound cards (built-in + USB). The test signal
needs to be routed specifically to the card wired into the decoder's
ADC via the audio loop, bypassing the built-in speakers.

---

## [B246 — Dual IQ path + 3-way switch (Stage 3.1)] - 2026-04-16

### Added
**A second IQ branch** in `src/dsp_pipeline.cpp`. The signal is now
demodulated in parallel along two paths after the LMS-notch:
- **Path A** (existing, narrow): biquad LPF BW = `baud · tuning_lpf_k`
  (≈0.75·baud).
- **Path B** (new, wide): biquad LPF BW = `baud · 1.5` — wider band,
  more drift/ISI tolerance, a touch more noise.

Both branches run all the time (so switching is click-free). The
power-pair (mark/space) feeding the framer is selected by
`shared_decoder_path`:
- `0 = A` (narrow) — default, identical behaviour to B245.
- `1 = B` (wide).
- `2 = HYB` — simple average `0.5·(A+B)` (Stage 3.2 will replace this
  with SNR-weighted fusion).

### UI
- **Menu → PATH button** (3rd column of the bottom row): tap cycles
  A → B → HYB → A. Colour is constant (muted blue) — only the label
  changes.
- **Top bar row 3** under `ST:` — indicator `P:A` (dim) / `P:B` (green)
  / `P:HYB` (cyan).
- Selection persists in flash (`AppSettings.decoder_path`).

### Serial
- `PATH A` / `PATH B` / `PATH HYB` — explicit switch.

### Measurement plan
Next sweep: three separate runs (A/B/HYB) through the simulator under
identical conditions — compare threshold and CER at −8..−12 dB.
Expectations:
- A baseline −10 dB (regression check vs B245).
- B: maybe slightly worse in AWGN (wider BW), better under drift.
- HYB: between them, dumb average with no intelligence; Stage 3.2
  should beat it.

### Next
Stage 3.2 — replace the dumb average with weighted fusion + SNR
estimate.

---

## [Phase 9 Progress Report — Stages 1-2 closed] - 2026-04-16

Full detailed write-up: `docs/PHASE9_PROGRESS_REPORT.md`.

### Summary table
| Build | Stage | Threshold | Quality | Status |
|-------|-------|-----------|---------|--------|
| B230 | baseline | −10..−11 dB | baseline | — |
| B242 | 1.1 Soft-LLR | −10 dB | −14 dB alive (0→282 chars) | ✅ |
| B243.1 | 1.2 Soft-Viterbi | −10 dB | −8 dB 6% → 0% | ✅ |
| B244 | 2.1 LMS-notch | −10 dB (AWGN) / +1-2 dB (QRM) | Stable under CW | ✅ |
| B245 | 2.2 Input BPF | −10 dB | −10 dB 15% → 0% (clean edge) | ✅ |

### What I actually got out of Stages 1-2
- The AWGN threshold **did not shift** (−10 dB) — but that's
  **expected**: Stages 1-2 are preparation work, an honest framer plus
  QRM resilience plus band hygiene.
- The framer no longer produces false frames at the edge (B243).
- CW QRM resilience (B244) — a new property.
- Clean input for fusion (B245).

### The real threshold push starts at Stage 3
- Stage 3 (two-IQ fusion): +0.5-1.5 dB.
- Stage 4 (spectral NR): +1-2 dB.
- Stage 5 (ML post): +1-2 dB.
- Goal: threshold **−14..−15 dB**.

### Caveats on the measurements
- `cer_analyze.py` sometimes shows phantom 7-8% at high SNR thanks to
  single byte-losses on the serial — read it over the solid 0% range.
- The `=NN=` markers occasionally vanish (LMS-notch cold-start) and a
  neighbouring bin captures the content, inflating CER artificially.
- Everything is measured in synthetic AWGN through simulator→ADC. Real
  air will add selective fading, impulses, TX drift — Stages 4-5 may
  paint a different picture on a real dataset (task #16).

---

## [B245 — Input BPF 300-3000 Hz (Stage 2.2)] - 2026-04-16

### Added
**Fixed Butterworth BPF 300-3000 Hz** — two biquads (HPF@300 + LPF@3000)
inserted after AGC, before the LMS-notch. Helper `design_hpf()` added
to `src/dsp/biquad.hpp` (previously LPF-only).

### Why
Phase 9, Stage 2.2. The pipeline plan: `AGC → BPF → LMS-notch → IQ`.
The BPF complements the 63-tap FIR (already a bandpass) — it cuts
residual DC/hum below 300 Hz and HF noise above 3 kHz that the FIR lets
through. The main goal is to clean up the band before the LMS-notch and
the IQ demod so that Stage 3 (fusion) gets a clean input.

### Measured (2026-04-16, AWGN only)
| SNR  | B243.1 | **B245** |
|------|--------|----------|
| +14..−8 | 0-2% | 0% ✓ |
| **−10** | ~15%* | **0.00%** ✓ |
| −12  | 9%* | 31%* |
| −14  | 25.9% | lost |

\* bins capture an adjacent SNR due to drifted markers.

**Threshold: −10 dB** (same as B243.1/B244), but bin −10 went fully
clean (0.00% vs 15% on B243.1). The edge of the threshold is cleaner.

AWGN-neutral on threshold, as expected for a fixed BPF: the real BPF
win will surface in QRM/noise-floor tests and as clean input for
Stage 3 (fusion).


### Next
Stage 3: Fusion of the two IQ branches (narrow LPF + wide raised-cosine)
with weighted combine. Expected gain +0.5-1.5 dB — the first stage that
should actually move the threshold down.

---

## [B244 — LMS-notch adaptive (Stage 2.1)] - 2026-04-14

### Added
**New module `src/dsp/lms_notch.hpp`** — 2nd-order constrained adaptive
notch (Nehorai-style). Wired into the pipeline as a cascade of two
instances directly after AGC, before IQ demod.

- **Low notch**: window 300–1350 Hz, start 600 Hz. Catches CW QRM
  below the RTTY band.
- **High notch**: window 1650–3200 Hz, start 2200 Hz. Catches QRM above.
- Pole radius `r = 0.985` → BW ≈ 48 Hz (narrow null, doesn't damage
  neighbouring tones).
- LMS step `mu = 5e-6` — conservative, convergence in ~1-2 s.
- Coefficient `a` clamped to a permitted range so that the null can't
  wander into the RTTY band (1400..1600 Hz) and so the two notches
  can't converge onto the same interferer.

### Why
Phase 9, Stage 2.1. In clean AWGN the gain ≈ 0 (the notch has nothing
to converge on); in real-air with CW QRM I expect +1-2 dB on threshold.
The main goal is to push the decoder toward −15..−16 dB under
narrowband interference.

### How to measure
- **AWGN-only sanity**: same sweep, CER must not rise.
- **QRM test**: in the simulator turn on CW QRM at 1000 Hz, level
  −10 dB, sweep. Without the notch −5 dB is death; with the notch I
  hope for −8..−10 dB threshold.

### Cost
- ~2 MAC × 2 notches × 10 kSps = 40 kMAC/s on Core 0. Negligible.
- Stability: form `1 + a·z⁻¹ + z⁻²` with poles on r < 1 — always
  stable.

### Measured (2026-04-14)

**AWGN-only (sanity)**: threshold −10 dB (same as B243.1). Minor noise
at +14 dB from notch cold-start (not enough time to converge before the
high-SNR sample).

**AWGN + CW QRM** (operator level, frequency outside RTTY band):
threshold −10 dB **unchanged**, from +20 to −8 dB everywhere ≤2% CER.
The notch successfully nulls the QRM — without it CW usually shreds
the decoder even at high SNR.

Subjectively +1-2 dB gain in QRM conditions, as planned.

### Tools changes
`tools/cer_analyze.py::best_cer` optimised: was O(49·N²) (Levenshtein on
every cyclic GT shift), now O(49·N + 3·N²) — a coarse char-match picks
top-3 shifts, then Levenshtein runs only on those. On logs with large
bins (merged due to lost markers) the speedup is 10-50×.

### Next
Stage 2.2: Input BPF 300-3000 Hz — protection against the high-frequency
white-spectrum trash the current anti-aliasing FIR lets through.

---

## [B243 — Soft-Viterbi framer (Stage 1.2)] - 2026-04-14

### Changed
**Framer in `src/dsp_pipeline.cpp` + `src/dsp/dpll_framer.hpp`**: on
top of the B242 soft-LLR, two soft-bit gates are added at the frame
boundary:

- **Weakest-link (data-bit)**: reject the frame if
  `min(|soft_data[i]|) < 0.20·sig_level`. Filters the case where one of
  the 5 data bits ended up near zero — then the slice on sign was a
  coin flip and the resulting Baudot code was random. That's exactly
  what produced 6% CER at -8 dB after B242.
- **Frame-average**: reject if
  `mean(|start| + |data[0..4]| + stop) / 7 < 0.30·sig_level`. Cuts
  frames with weak overall statistics (a low-SNR window).

### Why
Phase 9, Stage 1.2 — the second half of soft-decision. B242 only
validated the frame edges (stop/start), but the internal data bits were
still getting a hard-slice with no confidence check → false frames at
the SNR edge.

### Tuning (B243.1)
The first measurement with 0.20/0.30 thresholds **regressed**: at
+20 dB CER=4.94% (clean frames being rejected), at -8 dB CER=28%, at
-10..-14 the decoder died. Thresholds loosened to 0.10/0.15 —
sensitive soft-bit gate, not paranoid.

### Measured (2026-04-14, AWGN only, B243.1 thresholds=0.10/0.15)

| SNR | B230 | B242 | B243.1 |
|-----|------|------|--------|
| +20..−6 | 0-2% | 0-6% | **0-2%** |
| **−8** | ~0% | **6.0%** | **0.00%** ✓ |
| −10 | 0.6% | 1.8% | ~15%* |
| −12 | 9.1% | 9.4% | — (marker damaged) |
| −14 | lost | 25.9% | 27.2% |

\* bin −10 dB captured the content of −12 dB because the `=17=`
marker went missing in the decode stream.

**Threshold (CER≥5%): −10 dB** — same as B230/B242, but the edge is
clean.

**Headline win**: false frames at −8 dB (6.0% B242 → 0.00% B243.1)
cleaned up by the weakest-link gate. That's what Stage 1.2 was
supposed to deliver.


### Next
Stage 1.2 closed. Moving to Stage 2 — the noise environment:
- **Stage 2.1**: LMS-notch (2 adaptive nulls) against CW QRM.
- **Stage 2.2**: Input BPF 300-3000 Hz.
Expected gain: +1-2 dB on real air (nothing in clean AWGN).

---

## [B242 — Soft-LLR bit decision (Stage 1.1)] - 2026-04-15

### Changed
**Framer in `src/dsp_pipeline.cpp`**: the hard-slice at the bit boundary
(`integrate_acc > 0`) replaced with Soft-LLR plus an adaptive threshold
at the frame boundary.

- Keep `soft_start`, `soft_data[5]`, `soft_stop` (= last
  `integrate_acc`) — soft values, not bit decisions.
- EMA `sig_level = 0.98·sig + 0.02·|integrate_acc|` tracks signal level
  (adapts to AGC drift / M–S imbalance).
- On the stop bit: a frame is valid only if `soft_stop > 0.25·sig_level`
  **and** `-soft_start > 0.15·sig_level`. Previously the fixed
  `integrate_acc > 0` threshold accepted weak/zero bits as MARK →
  garbage at low SNR.
- The stop-gap arming for STOP-DET is now tied to `valid_stop`, not to
  the raw bit.
- Data bits still hard-slice into `current_char` (soft-Viterbi comes in
  Stage 1.2).

### Why
The Phase 9 plan, Stage 1.1 — the first cheap win in the chain to
−15..−16 dB. Expected gain +2–3 dB from replacing hard-slice with
adaptive-threshold frame validation.

### Measured (2026-04-15, AWGN only)

| SNR | B230 CER | B242 CER |
|-----|----------|----------|
| +18..−6 | ~0% | ~0% |
| −8  | ~0%  | **6.02%** ⚠️ |
| −10 | 0.60% | 1.79% |
| −12 | 9.09% | 9.38% |
| −14 | marker lost | **25.89%** (282 chars) |

**Threshold (CER≥5%): ~−10..−11 dB — unchanged vs B230.**

Observations:
- At −14 the decoder no longer dies (282 chars vs lost marker) — the
  adaptive threshold lets more frames through.
- But at −8 there's a strange spike of 6% — preview reads
  `QWERTYUIOP RYRYRY...`, looks like a false frame that slipped past
  the softened threshold.
- Without soft-Viterbi (Stage 1.2) adaptive thresholds alone aren't
  enough — more characters, but also more garbage.


### Next
- Stage 1.2: Soft-Viterbi framer with stop-bit as a constraint —
  should filter out garbage frames via soft decisions over the 5 data
  bits.
- Maybe: tweak `STOP_MIN_FRAC` / `START_MIN_FRAC` — but that's tuning,
  not architecture.

---

## [Baseline Build 230 — AWGN only] - 2026-04-15

### Measured
First honest baseline measurement of Build 230 via sync markers
(`--markers`).

| SNR (dB) | CER     |
|----------|---------|
| +18..−8  | ~0%     |
| −10      | 0.60%   |
| **−12**  | **9.09%** |
| −14      | marker lost |

**Decoder threshold (CER≥5%): ~−10..−11 dB** — 4 dB better than the
plan said (−6..−8). But conditions are ideal: AWGN only, no
QRM/drift/fading/impulse. Artifacts in
`docs/baseline_build230_{cer.csv,sweep.txt,serial.log}`.

### Fixes in `cer_analyze.py`
- `clean_decoded()`: strips `[FIGS]`/`[LTRS]`/`[ERR]` tags before
  Levenshtein. Before the fix CER was ~40% on a clean decode — the
  tags counted as insertions.
- Threshold estimate: ignores empty bins (0 chars) so it doesn't lie
  about "threshold=+20 dB" when the first sample simply didn't fall
  between markers.

### Phase 9 goal
Beat 2Tone: threshold **−15..−16 dB** (4-5 dB better than baseline).
Starting Stage 1.1 — Soft-LLR bit decision (+2-3 dB expected gain).

## [Build 241] - 2026-04-15
### Added (sweep sync markers — robust CER binning)
- **Simulator**: in `startSweep()`, on every SNR step a marker `=NN=`
  (NN = the two-digit point index, padStart `01`..`18`) is injected
  into the main RTTY stream. Implemented via a modular `markerQueue`
  which `scheduleChunk()` consumes **before** the main text;
  `charIndex` doesn't advance while a character is taken from the
  queue.
- **Marker format**: `" =NN= "` with space separators. `=` only
  appears in FIGS, so the regex `=\d\d=` in the decoded stream doesn't
  clash with the `RYRYRY...` of the main text.
- **`cer_analyze.py --markers`**: a new mode. Concatenates the whole
  serial output, finds every `=NN=` via regex, splits the stream into
  segments between markers and maps them to sweep points by number.
  Resilient to the serial-output batching (which is exactly what
  broke the B230 baseline).
- The sweep log now additionally writes `MARK==NN=` in each point's
  line for tracing.

### Why
The B230 baseline measurement showed: the device's serial output
arrives in large chunks with one timestamp per chunk. Timestamp
binning doesn't work. Inline markers in the audio signal itself are
the cleanest solution — no shared clock, no firmware changes. At very
low SNR the markers get lost too, but at that point CER is 100%
anyway — not critical.

## [Build 240] - 2026-04-15
### Added (simulator — noise preview + impulse tone/duration controls)
- **`NOISE ONLY` button** in `rtty_simulator.html`: runs the full
  audio chain (AWGN, CW, QRM-RTTY, impulses, fading, drift) WITHOUT
  the main RTTY transmitter. `markBitGain` stays at 0. Needed so I
  can sanity-check by ear that every interference type actually
  sounds right and tunes correctly.
- **Impulses — new controls**:
  - `Tone (Hz)` 100..4000 — central click frequency.
  - `Duration (ms)` 1..40 — burst length.
  - Checkbox `Random tone + duration per burst` — every impulse
    picks a random tone (150..3650 Hz) and duration (1..16 ms).
- **Changed impulse shape**: was — decaying white noise 2 ms. Now —
  damped sine `env·(0.7·sin(2πf·t) + 0.3·noise)`, τ=len/4. Sounds
  like a natural atmospheric/discharge, not just "psh".
- Refactored `startRTTY()` → `startRTTY(muteMain)` + global `rttyMuted`.
  Status line in noise-only shows yellow "NOISE ONLY (main RTTY muted)".

### Why
I noticed the impulses are inaudible against RTTY. The noise-only
button lets each interferer be isolated. Tunable tone/duration makes
the impulses more realistic (atmospherics vary: grain-level, lightning,
regulator spikes).

## [Build 239] - 2026-04-13
### Added (testbench — Python tools — **closes the testbench phase**)
- **`tools/cer_analyze.py`**: correlates the HTML sweep log + serial
  log → CER(SNR) table.
- **Algorithm**:
  1. Parse sweep log — extract (ISO_ts, SNR, idx) for every point.
  2. Parse serial log — timestamp + text per line.
  3. For every sweep point: window `[ts+trim, next_ts)`, collect
     decoded chars.
  4. Clean: ASCII printable only, uppercased.
  5. **Best cyclic Levenshtein** vs ground truth: tries every offset
     in the GT cycle, takes the minimum. Normalization:
     `cer = edit_distance / len(decoded)`.
- CLI: `--sweep --serial --gt <str|@file> --out <csv> --plot <png> --trim <sec>`.
- **Threshold estimate** heuristic: the highest SNR at which CER ≥ 5%.
  This is my main metric (decoder threshold).
- Optional matplotlib CER-vs-SNR plot.
- Smoke test passed on synthetic logs: 0% → 67% CER, threshold
  correctly found.

### Phase 9 testbench ready
Every tool for objectively measuring the decoder is in place:
- HTML simulator (AWGN, QRM, drift, fading, morse, sweep).
- Python offline (rtty_gen, serial_logger, cer_analyze).

Next step: **baseline measurement of the current Build 230 decoder**,
then Stage 1.1 (soft-LLR).

## [Build 238] - 2026-04-13
### Added (testbench — Python tools)
- **`tools/serial_logger.py`**: timestamped logger of the device
  serial output. Each incoming line is written as
  `<ISO8601>\t<line>`, compatible with the HTML sweep-log timestamps.
  Used in pair with the sweep from `rtty_simulator.html` →
  `cer_analyze.py` (next build) matches by time.
- Dependency: `pyserial` (already installed).

## [Build 237] - 2026-04-13
### Added (testbench — Python tools)
- **`tools/rtty_gen.py`**: offline WAV generator with known text +
  SNR control. CLI args:
  `--text --baud --shift --stop --center --snr --sr --duration --out`.
- ITA2 Baudot (LSB first, start=Space, stop=Mark), FIGS/LTRS
  auto-shifts, matches `rtty_simulator.html` conventions.
- Continuous-phase synthesis (phase carried across frequency edges —
  no discontinuities like `OscillatorNode` retunes).
- AWGN in the audio band: `noise_rms = signal_rms · 10^(−SNR/20)`.
  Automatic clip-protection.
- Ground-truth text printed to stdout (for cross-checking against the
  decoded serial output).
- Smoke test passed: 3 s @ +5 dB SNR → 16 chars "RYRYRY RYRYRY RY".
- Dependencies: `numpy`, `scipy` (installed).

## [Build 236] - 2026-04-13
### Added (testbench — Phase 9, item 5/5 — closes the testbench phase)
- **Batch SNR sweep in `rtty_simulator.html`**. Automatic SNR sweep
  through given points with dwell time. UI:
  - Sliders: `SNR from` +30..−25, `SNR to` +30..−25, `Step` 1..5 dB,
    `Dwell` 5..120 s.
  - Buttons `SWEEP` / `CANCEL`.
  - Log area: ISO timestamp + SNR + index per point. First/last line
    = sweep boundary markers.
- The sweep automatically turns on AWGN and moves the SNR slider.
  Direction (up/down) inferred from the sign of the difference.
- **CER(SNR) measurement methodology**: (1) start TX in the
  simulator, (2) start the device serial logger with timestamps,
  (3) press SWEEP, (4) copy the log text + serial log, (5) offline
  Python (next task) matches by time and computes CER per point.
- **Closes the testbench phase of Phase 9**. Next: Python
  `rtty_gen.py` + `cer_measure.py` for offline reproducibility, then
  the baseline measurement of the current decoder.

## [Build 235] - 2026-04-13
### Added (testbench)
- **CW QRM now uses real Morse**. Added dropdown
  `CW mode: Continuous carrier | Keyed morse`. In keyed mode — full
  Morse-coded transmission of `"CQ CQ DE UA3TEST K  "` with a
  character dict for A-Z/0-9/punctuation.
- **Hand key** (realism): jitter slider `0..50 %` adds random scaling
  to the length of every element (dot/dash/pause). Realistically
  imitates an operator, not a perfect machine. At 20% jitter the
  dot/dash ratio floats, inter-letter pauses float too. WPM slider
  `10..40` (PARIS-based: dot = 1.2/WPM).
- **Envelope**: 5 ms linear ramp on element on/off — a soft key-click
  (not perfectly sharp, as with a real key with a side-tone filter).
- Separate scheduler (`scheduleCWChunk`), 1.5 s look-ahead, recovers
  `cwTime` if it falls behind.

## [Build 234.1] - 2026-04-13
### Fixed
- **QRM RTTY scheduler wasn't starting**: in `startRTTY` the
  `scheduleQRMChunk()` call was placed **before** `isPlaying = true`,
  and the function returns at the top on `if (!isPlaying) return;`
  with no rescheduling. The second RTTY signal ended up stuck on the
  initial Mark frequency (sounded like a continuous tone). Call
  moved after `isPlaying = true`.

## [Build 234] - 2026-04-13
### Refactored
- **Simulator signal path → dual-tone (independent Mark/Space
  branches)**. Instead of one `OscillatorNode` with
  `frequency.setValueAtTime` switching — two persistent branches:
  `markOsc → markBitGain → markFadeGain → gain` and
  `spaceOsc → spaceBitGain → spaceFadeGain → gain`. The scheduler now
  toggles the BitGains (with a 0.5 ms micro-ramp for anti-click)
  instead of the frequency. The drift branches are connected to
  **both** `osc.frequency` (ConstantSource + SinOsc →
  markOsc.frequency + spaceOsc.frequency), i.e. both tones drift in
  sync.
- **Why the refactor**: you can't honestly model selective fading
  (where Mark and Space fade independently — HF multipath) without
  separate gain on each carrier.

### Added (testbench — Phase 9, item 4/5)
- **QSB — flat amplitude fading**: a sinusoidal envelope on top of
  the whole signal. Sliders `depth 0..40 dB` and `period 1..60 s`.
  Formula: `fade = 10^(−(depth/2)·(1−cos(2π·t/T))/20)` ∈
  [10^(−depth/20), 1] — periodically dips to the minimum and returns
  to 1.
- **Selective fading** (HF multipath): Mark and Space fade
  **independently**, with a 0.7π phase shift between them. Mimics
  the situation where one tone is deep in a null and the other is
  visible — typical on HF under ionospheric multipath. Sliders
  `depth 0..40 dB`, `period 1..30 s`.
- Envelope update in JS via `setInterval(50 ms)` +
  `setTargetAtTime` on `markFadeGain`/`spaceFadeGain` (cheap, QSB is
  slow).
- **Impulse noise** (QRN / atmospherics): random short (~2 ms)
  exponentially decaying noise bursts. Poisson distribution:
  intervals `−ln(1−rand) · 60/rate`. Sliders
  `rate 0..300 clicks/min`, `amplitude ×0..×20` of SIGNAL_PEAK.
- **Use case**: conditions as close as possible to real HF air for
  stress-testing the decoder (current and future hybrid).

## [Build 233] - 2026-04-13
### Added (testbench — Phase 9, item 3/5)
- **tools/rtty_simulator.html: frequency drift** of the main signal.
  Two independent components, summed on the `osc.frequency`
  AudioParam (on top of `setValueAtTime` scheduling, because
  AudioParam sums intrinsic + inputs):
  - **Linear drift**: `ConstantSourceNode` with a long
    `linearRampToValueAtTime` (rate·3600 per hour). Slider
    `−10..+10 Hz/s`, step 0.1. Mimics a TRX thermal walk after
    power-on.
  - **Sinusoidal drift**: low-freq `OscillatorNode` × `GainNode` →
    amplitude in Hz. Sliders `amp 0..50 Hz`, `period 1..60 s`.
    Mimics Doppler / ionospheric wobble / QSB frequency.
- Both branches with live update via `setTargetAtTime`. Checkboxes
  enable/disable independently.
- **Use case**: AFC and SEARCH resilience to slow frequency drift;
  preparation for evaluating how much better a widely matched
  filter (path B from §3 of the plan) is than a narrow one (path A)
  under drift.

## [Build 232] - 2026-04-13
### Added (testbench — Phase 9, item 2/5)
- **tools/rtty_simulator.html: QRM injection**. Two parallel
  interference branches on top of the main RTTY signal:
  - **CW carrier**: a continuous sine tone. Frequency slider
    `300..3000 Hz` + level `−30..+20 dB` relative to the main signal.
    Live update without restart.
  - **Second RTTY**: a second 45.45/170/1.5 RTTY signal with the
    fixed text `"CQCQCQ DE TEST RYRYRY 73 "`. Center-freq slider
    `400..2800 Hz` + level `−30..+20 dB`. Separate scheduler with
    its own look-ahead (1.0 s).
- **Levels**: both QRM sources normalized to `SIGNAL_PEAK=0.5`, so
  `cw_gain = 0.5·10^(lv/20)`. `lv=0 dB` = same power as the main
  signal.
- **Use case**: SEARCH and decoder resilience to neighbouring
  signals, imitation of real-air with several stations sharing the
  band.

## [Build 231] - 2026-04-13
### Added (testbench — Phase 9, item 1/N)
- **tools/rtty_simulator.html: AWGN + SNR slider**. First step
  toward a testbench for the hybrid decoder (Phase 9). Added a
  parallel branch of white Gaussian noise to the simulator (sum of
  3 uniform, RMS≈1.0, 10-second looped buffer). Noise gain is
  computed as `SIGNAL_RMS · 10^(−SNR/20)` — slider `−25..+30 dB`
  changes the noise level live, no restart. The "AWGN" checkbox
  enables/disables the branch.
- **Convention**: sine peak = 0.5 (RMS ≈ 0.354), SNR in the full
  audio band (not in bit bandwidth). A simplification for an
  interactive test; the precise in-band SNR is computed later in
  Python.
- **Use**: drag the slider, watch at what SNR the decoder
  (Build 230) starts to fall apart — get a first cut at the
  baseline threshold before introducing soft-LLR.
- Plan: separate builds will add QRM injection, frequency drift,
  impulse noise, batch mode for CER measurement.

## [Build 230] - 2026-04-13
### Verified (no code changes)
- **STOP-DET algorithm confirmed on real air (50/450/1.5)**: a
  60-second capture via `C:\Temp\stopdet_capture.ps1` with a correct
  SEARCH lock (FREQ=984.1, ERR=3%) showed an unambiguous vote
  `Result: 1.5 bits (votes: 1.0=0 1.5=19 2.0=1)`. All measured
  gap_fractions are in 0.34–0.60T — squarely inside bin 1.5
  (boundaries 0.25/0.85 are correct).
- **The earlier bug** ("detected 1.0 instead of 1.5") was a
  downstream symptom: SEARCH wasn't locking properly, so the
  state-7-end timing was measured on a desynchronized framer. With
  a correct lock, STOP-DET works as designed.
- TODO: catch edge cases on other signals (100 baud, 2.0 stop,
  weak SNR).

## [Build 229] - 2026-04-13
### Fixed (measurement)
- **Core 1 load metric is honest now**: DMA waits in
  `ili9488_push_*` (`dma_channel_wait_for_finish_blocking`) were
  being counted as work even though they're blocking waits
  (Cortex-M33 is asleep). Added `shared_c1_dma_wait_time` — it
  accumulates DMA waits and is subtracted from Core 1 total work
  before the percentage is computed.
- **Result**: Core 1 measurement dropped from 41-47% to **8-10%**
  (matches the reference project). The compute load was always low
  — only the metric was lying. The real headroom for the hybrid
  decoder is huge.

### Changed
- UI update interval 200 ms → **500 ms** (as in the neighbouring
  project). Cuts top-bar repaints by 2.5×. Cosmetic; the real win
  is masked by the metric fix.

## [Build 228] - 2026-04-13
### Optimized
- **Incremental text rendering (Core 1)**: a regular char-append now
  redraws only the bottom line (`drawRTTYLastLineOnly`) — fillRect
  440×line_h + a single drawString + push_colors only of the
  affected strip, instead of fillSprite 480×160 + 16×drawString +
  full push. On the hot path (60 chars out of 61 before a wrap) it
  saves ~10× SPI traffic and ~10× render work. The full `drawRTTY`
  is called only on a new-line add (newline/CR/line-wrap), a
  scroll_offset change, or a screen re-render. Throttle 8 ms
  (~120 fps cap) on incremental.

## [Build 227] - 2026-04-13
### Optimized
- **Ring buffer FFT collection (Core 0)**: dropped the 2 KB
  `memmove` every 480 samples (~102 ms) — `ts[]` is now circular
  with a bitmask index `& (FFT_SIZE-1)`. The snapshot into
  `shared_fft_ts` is done by a single unwrap pass from
  oldest→newest instead of memmove + memcpy.
- **ADC-pacing via `adc_fifo_get_blocking()` with no busy-wait**:
  dropped the redundant
  `while(adc_fifo_is_empty()) tight_loop_contents()` before the
  real blocking call. Cortex-M33 now genuinely sleeps between
  samples. Timestamp `st` moved to *after* wake-up — the Core 0
  load metric excludes idle/sleep time.
- **Result**: Core 0 7% → **4-5%**. Approaching the reference
  project (3%). The ring FFT also reduces I-cache pressure.

## [Build 226] - 2026-04-13
### Fixed
- **SEARCH no longer skips wide FSK pairs**: the B222 valley-test
  (rejecting pairs with a deep dip between peaks) was too
  aggressive — it was rejecting a legitimate 450 Hz signal
  (Mark b180=+41 dB, Space b224=+22 dB, valley on the noise floor
  ~−2 dB, diff 33 dB). Threshold bumped 25 → 40 dB: real wide-FSK
  (valley ~30-35 dB below peaks) passes, cross-signal false combos
  (diff 40+ dB) are still rejected. Verified on live air: the
  weather 50/450/1.5 is picked with score=109.8, beating noisy
  200-Hz candidates.
- **SEARCH per-shift tolerance (inner loop)**: B222 widened
  `local_tolerance` only in the outer loop (lo boundaries). The
  inner `for (d = -tolerance; ...)` stayed on the constant 2 — so
  for 425/450/500/850 a fraction of legitimate candidates with
  drift ±3-4 bins were still being rejected. Now both loops use
  `local_tolerance`.

### Added
- **DUMP SPEC**: a serial command that dumps the current FFT
  magnitudes (512 bins, bin = 9.77 Hz). Needed for offline spectrum
  analysis — pull a slice from the device and figure out, in
  dialog, what signals are present, why AUTO locked on the wrong
  pair, etc.
- **DUMP MS**: dumps the Mark/Space envelopes (480 samples of
  history).
- **shared_fft_mag[]**: Core 1 copies `smooth_mag` into a shared
  array after every FFT computation. Needed for DUMP SPEC (the
  serial handler doesn't FFT itself).

## [Build 222] - 2026-04-13
### Added
- **Valley test in SEARCH**: rejects fake FSK pairs from two
  independent signals whose accidental frequency difference matches
  a standard shift. Example: a narrow CW at 890 Hz + a strong Mark
  of a wide RTTY at 1758 Hz → difference 868 Hz ≈ 850 shift →
  SEARCH picks a false 850. The minimum magnitude between the peaks
  is checked for shift > 20 bins; on a very deep dip (>40 dB below
  the peaks, calibrated in B226) the pair is rejected as "two
  different signals".

### Changed
- **Wider shifts have wider tolerance**: at shift_bins ≥ 15
  tolerance=3, at ≥ 40 tolerance=4 (was a constant 2). Compensates
  for FSK spectral smearing and TX drift on wide shifts — a
  450 Hz signal can sit at 44 bins instead of the ideal 46 and not
  be lost.

## [Build 221] - 2026-04-12
### Added
- **Seqlock for shared DSP data**: Core 0 wraps the write of
  `shared_fft_ts/adc_waveform/mag_m/mag_s` in an increment of
  `shared_dsp_seq` with `__dmb()` barriers. Core 1 reads with a
  retry loop (up to 3 attempts) — if the seq changes between the
  start and end of memcpy, the data is treated as torn and re-read.
  Groundwork for the future move of the FFT to Core 0 (the shared
  update rate goes up).
- **SAVE flash serial indicator**:
  `[SAVE] writing flash (DSP paused ~45ms)...` + `[SAVE] done in
  X me`. The UI SAVE button already changes colour visually.

### Changed
- `__dmb()` memory barriers added in the Core 0 writer and the
  Core 1 reader for correct seqlock operation on the dual-core ARM.

## [Build 220] - 2026-04-12
### Optimized
- **63-tap symmetric FIR**: power-of-2 (64) buffer for bitmask
  indexing instead of `% 63`. Coefficient symmetry exploited
  (`fir_coeffs[i] == fir_coeffs[62-i]`) — 32 multiplies + 31 pair
  adds instead of 63 multiplies. Forward iteration drops the
  reverse branch.
- FIR ~50% faster, frees ~0.5% of Core 0.

## [Build 219] - 2026-04-12
### Added
- **PIO Waterfall LUT**: a precomputed `waterfall_pio_lut[256]`
  rainbow-gradient table (uint8 → 32-bit PIO-ready RGB666). The
  rainbow computation is now an O(1) lookup instead of 6 float ops
  + color565 + byte swap on every one of the 480×64 = 30720 pixels
  in a frame.
- **Circular history buffer**: `wf_history[64][480]` uint8 (30 KB)
  instead of an RGB565 sprite (61 KB). Scroll = decrement
  `wf_offset` with no memcpy.
- A new function `ili9488_push_waterfall_lut()` — render via
  history + LUT + ping-pong DMA.

### Changed
- Core 1 lower-bound load: 60% → **39%**. Waterfall FPS:
  stable 22 → 20-25.
- Reference idea from `c:\YandexDisk\DIY\RP2350_RTTY\TouchRTTY\`
  ported (same PIO LUT + history buffer scheme there).

### Documented
- `docs/ROADMAP_OPTIMIZATION.md` section 8: hybrid RTTY decoder
  (goal — **beat 2Tone**, threshold ~−15..−16 dB SNR). 4 stages:
  Goertzel matched filter → multi-phase Goertzel → character-level
  ML → Bayesian prior + Viterbi + noise blanker + spectral sub +
  temporal diversity + tiny NN fallback + soft confidence UI.
- `docs/20260412/` — detailed algorithm analysis
  (RTTY_DECODER_ALGORITHMS_COMPARISON, IQ_VS_GOERTZEL_ML_ANALYSIS,
  OPTIMIZATION_AND_INTERFERENCE_MITIGATION).

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
