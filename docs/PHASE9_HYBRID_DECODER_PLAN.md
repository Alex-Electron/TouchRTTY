# Phase 9 — hybrid RTTY decoder plan

> 🇷🇺 [Читать на русском](PHASE9_HYBRID_DECODER_PLAN.ru.md)

> **Status: SHIPPED as v2.0.0 (Build B265, 2026-05-12).**
> This document is the frozen design plan that drove the
> implementation. The architecture partly diverged from the plan (see
> §0 and `docs/ROADMAP_OPTIMIZATION.md` §8): I went with **dual-IQ +
> LLR fusion + TinyML NN** instead of Goertzel + Character-ML. The
> result — decoding threshold ≈ −16 dB SNR (CER ~9 pp) vs 2Tone's
> ~−13 dB, which closes the "better than 2Tone" goal. Full results in
> `RELEASE_v2.0.0.md` and `docs/NN_TRAINING.md`.

**Created:** 2026-04-13
**Status:** historical design doc (plan shipped, see banner)
**Goal:** a decoder that pulls RTTY at −15..−16 dB SNR (better than
2Tone, current threshold ≈ −6..−8 dB).

---

## 0. Clarification — what's actually in the code right now

The confusion was justified. Above I sloppily called "Goertzel" what's
actually already **IQ demodulation** in code. Real state of
`src/dsp_pipeline.cpp`:

```
f_out (AGC output)
  ├─► × cos(2π·f_mark·t)  → biquad LPF → mi
  ├─► × sin(2π·f_mark·t)  → biquad LPF → mq
  ├─► × cos(2π·f_space·t) → biquad LPF → si
  └─► × sin(2π·f_space·t) → biquad LPF → sq

mark_power  = mi² + mq²
space_power = si² + sq²
```

That's a classic **quadrature (IQ) demod** through NCO (sin/cos tables
of 1024) + biquad LPF on each branch. Formally equivalent to a
sliding-window Goertzel at the same LPF bandwidth, but cheaper and
with a flexible response (set by biquad coefficients).

**Why I confused you:** in the plan I wrote "Dual-Goertzel" out of
habit — in the literature narrowband tone detection is often called
Goertzel. But I already have an IQ path, and that's a good thing:
biquad LPF gives a better magnitude response than rectangular-window
Goertzel.

**So what does "hybrid" mean here:** not Goertzel vs IQ, but **two
parallel IQ branches with different LPF characteristics**, combined
through fusion logic. See §3.

---

## 1. Processing stages (final chain)

```
ADC 10 kHz
  │
  ├─► [A] DC-block + AGC                     [exists]
  │
  ├─► [B] Input BPF 300–3000 Hz              [new, cheap]
  │
  ├─► [C] Adaptive LMS notch (2–3 nulls)     [new]
  │
  ├─► [D] Spectral noise reduction            [new, optional]
  │       (spectral subtraction via FFT path)
  │
  ├─► [E] IQ demod path A: narrow LPF        [refactor of current]
  │       (BW ≈ baud · 1.0, minimum ISI)
  │
  ├─► [F] IQ demod path B: wide LPF          [new]
  │       (BW ≈ baud · 1.5, matched raised-cosine)
  │
  ├─► [G] Fusion — weighted combine A+B       [new]
  │       by SNR/drift estimate
  │
  ├─► [H] Soft-LLR bit decision               [new, critical]
  │       LLR = (M_env² − S_env²)/σ²
  │
  ├─► [I] DPLL bit sync on LLR                [refactor: hard → soft]
  │
  ├─► [J] Soft-Viterbi framer (5N1.5/2)       [new]
  │       start=0, stop=1 as constraint
  │
  └─► [K] ML post-classifier (eye → symbol)   [optional, finale]
          small CNN, synthetic+real dataset
```

---

## 2. Improvement budget

| # | Stage | Expected gain | Difficulty |
|---|---|---|---|
| C | LMS notch | +1–2 dB (in QRM-heavy on-air) | low |
| D | Spectral NR | +1–2 dB (on weak signal) | medium |
| E+F+G | Fusion of two IQ paths | +0.5–1.5 dB | medium |
| H+J | Soft-LLR + Viterbi framer | +2–3 dB | high |
| K | ML post-classifier | +1–2 dB | high |
| | **Total potential** | **+6–10 dB** | |

Current threshold ≈ −6..−8 dB → target −15..−16 dB. With an honest
implementation, reachable.

---

## 3. What "hybrid" means — refined

Two parallel IQ demod branches:

- **Path A (narrow):** biquad LPF, BW ≈ baud. Optimal SNR on stable,
  drift-free signal. Sensitive to frequency offset.
- **Path B (wide/matched):** raised-cosine FIR, BW ≈ 1.5·baud. Robust
  against drift and timing jitter, slightly worse on thermal noise.

**Fusion** (stage G):

- Variant 1 (simple): weighted sum of envelopes, weights = f(SNR
  estimate).
- Variant 2 (advanced): branch selection by current drift/jitter
  metric.
- Variant 3 (ML-based): small classifier over 4 metrics → weights.

Start with variant 1, see if anything more elaborate is needed.

---

## 4. Implementation order (approved plan)

**Stage 1** (quick win, simple):

- [1.1] Soft-LLR bit decision (H) — replace hard-slice
- [1.2] Soft-Viterbi framer (J) — use stop-bit as a constraint
- **Expected gain:** +2–3 dB from these two patches.

**Stage 2** (noise environment):

- [2.1] LMS notch (C) — 2 adaptive nulls
- [2.2] Input BPF (B) — fixed
- **Expected gain:** +1–2 dB in real on-air.

**Stage 3** (fusion):

- [3.1] Second IQ branch with raised-cosine FIR (F)
- [3.2] Fusion logic (G) — weighted combine
- **Expected gain:** +0.5–1.5 dB.

**Stage 4** (NR):

- [4.1] Spectral subtraction over FFT (D)
- **Expected gain:** +1–2 dB.

**Stage 5** (ML):

- [5.1] Dataset collection: synthetic + WebSDR recordings + real RX
- [5.2] Train a CNN on the eye diagram (16×220 → symbol)
- [5.3] Inference on RP2350 (hand-rolled, no TFLite)
- **Expected gain:** +1–2 dB.

After each stage — measure threshold, update CHANGELOG, agree before
the next.

---

## 5. Measurement methodology

What I need before I start: a **reference testbench** for objective
gain evaluation.

- [5a] Script to generate synthetic RTTY + AWGN at a given SNR
  (Python, offline).
- [5b] Reference signal captures via WebSDR (different baud/shift,
  weather/amateur stations).
- [5c] "Play into the line" procedure (audio cable into ADC, or via
  USB-DAC) — repeatable test.
- [5d] Metric: character error rate (CER) as a function of SNR.
  Threshold = SNR at CER = 5 %.

Without this I'd be moving blind. First thing after the plan is
agreed — §5.

---

## 6. Architectural decisions to confirm

1. **Dual IQ path vs one improved path:** do I go ahead with fusion
   (stage 3) or is one branch with a good raised-cosine enough?
2. **ML runtime:** hand-rolled float32 inference, no external libs.
   CNN ≤ 8K parameters. Agreed?
3. **Dataset:** WebSDR recordings (you have access) + synthetic.
   Target volume: 10k real characters + unlimited synthetic.
4. **Core split:** Core 0 — DSP (A..I), Core 1 — framer/ML (J, K) +
   UI. Agreed?
5. **Feature flag:** the new chain behind a flag (`DIAG HYBRID ON/OFF`)
   for A/B comparison with the old chain. Agreed?

---

## 7. Open questions / risks

- **Timing budget:** Core 0 is at 5 % now. Stage 2–3 adds ~3–5 %.
  Stage 4–5 adds another ~10–20 %. Should fit, but needs to be
  measured.
- **Flash:** the ML model is 8K·4B = 32 KB plus code. I have room.
- **`sq_snr` calibration:** with soft decoding the old squelch logic
  can interfere. Need to rebuild on soft confidence.
- **Backward compatibility:** AUTO search and the framer currently
  rely on hard decisions. Need to migrate carefully.

---

## 7a. Execution log (one bullet = one build)

| Build | Date | Item | Status |
|---|---|---|---|
| 231 | 2026-04-13 | Testbench #1: AWGN + SNR slider in `rtty_simulator.html` | ✅ done |
| 232 | 2026-04-13 | Testbench #2: QRM injection (CW + second RTTY) | ✅ done |
| 233 | 2026-04-13 | Testbench #3: frequency drift (linear + sine) | ✅ done |
| 234 | 2026-04-13 | Testbench #4: QSB + selective fading + impulse + dual-osc refactor | ✅ done |
| 235 | 2026-04-13 | Testbench extra: CW keyed morse mode | ✅ done |
| 236 | 2026-04-13 | Testbench #5: batch-mode SNR sweep (closes testbench phase) | ✅ done |
| —   | 2026-04-15 | **Baseline Build 230 (AWGN only): threshold ~−10..−11 dB** | ✅ done |
| 237 | 2026-04-13 | Python: `rtty_gen.py` (offline WAV generator + AWGN) | ✅ done |
| 238 | 2026-04-13 | Python: `serial_logger.py` (timestamped serial capture) | ✅ done |
| 239 | 2026-04-13 | Python: `cer_analyze.py` + **testbench phase closed** | ✅ done |
| 240 | 2026-04-15 | Simulator: NOISE-ONLY button + impulse tone/duration/random | ✅ done |
| 241 | 2026-04-15 | Sweep sync-markers (=NN=) + `cer_analyze --markers` mode | ✅ done |
| 242 | 2026-04-15 | Stage 1.1: soft-LLR bit decision (adaptive stop/start thresholds) | ✅ done — threshold didn't move (~−10..−11), but at −14 the decoder is alive (282 chars vs lost). Waiting for Stage 1.2 to filter junk. |
| 243 | 2026-04-14 | Stage 1.2: Soft-Viterbi framer (weakest-link data + frame-avg) | ✅ done — at −8 dB 6 % B242 → 0 % B243.1 (false frames gone). Threshold −10 dB. Tuned thresholds 0.10/0.15. |
| 244 | 2026-04-14 | Stage 2.1: LMS adaptive notch (2-stage: 300-1350 / 1650-3200 Hz) | ✅ done — AWGN threshold −10 dB preserved; with CW QRM threshold −10 dB and 0 % CER from +8 to −8 dB. |
| 245 | 2026-04-16 | Stage 2.2: Input BPF 300-3000 Hz (HPF + LPF Butterworth) | ✅ done — AWGN threshold −10 dB, bin −10 became 0.00 % (B243.1 was 15 %). Neutral, ready for Stage 3. |
| —   | —    | Stage 3: Fusion of two IQ branches | pending |
| —   | —    | Stage 4: Spectral NR | pending |
| —   | —    | Stage 5: ML post-classifier | pending |

Each item — a separate commit with build number, CHANGELOG entry,
update of this table.

## 8. What I do right now

After agreement:

1. Build the testbench (§5) — 1–2 sessions.
2. Take baseline CER(SNR) for the current decoder.
3. Start Stage 1 (soft-LLR).

All changes behind a feature flag, A/B-measured gain after every
substage.

---

## 9. Roadmap after Stage 5 var.1 (BW sweep) — agreed 2026-04-20

**Global goal:** TouchRTTY should be **better than every public
decoder** on AWGN and real channel — better than 2Tone (−12..−14 dB),
fldigi (−9..−11 dB), MMTTY (−8..−10 dB). Target: honest threshold
**−15..−16 dB**.

### 9.1. Methodology finding (2026-04-20)

While working on the BW sweep (#37) I discovered: **the ground-truth
text had no CR/LF**, so the decoder buffered output for minutes and
flushed in one chunk → bin attribution broke → threshold at 5 % CER
shifted to max SNR in the sweep (an artefact, not a real threshold).

**Fix:** `GT_TEXT = "RYRYRY THE QUICK BROWN FOX JUMPS OVER 1234567890 \r\n"`
in the orchestrator, dwell 60 s.

**Implications for past tests:**

- Tests *with* PATH/DYN/CMD cycling (B256 NR, B257 avg3) — OK; serial
  commands gave natural flushes every 10–20 s.
- Tests *without* cycling (parts of baseline measurements, possibly
  parts of Stage 1–2 gain measurements) — suspect.

### 9.2. Priorities (in order)

**P0. Stage 5 var.1 — matched filter BW sweep (task #37, in_progress)**

- k ∈ {0.40, 0.50, 0.60, 0.75, 0.90}, SNR −10..−18, dwell 60 s, 3 runs.
- Orchestrator: `tools/bw_sweep_orchestrator.py`.
- Expected: winner at k = 0.50..0.60 on Path A (early data shows lead
  at k = 0.50 on high SNR, k = 0.60 at −16).

**P1. Plan B — revalidate baseline + Stage 3.3 (task #38)**

- DYN ON/OFF A/B via
  `serial_logger --cmd-cycle 10 --cmd-seq "ON=DYN ON|OFF=DYN OFF"`,
  SNR −10..−18, 3 runs.
- Goal: confirm that ~−14 dB threshold and +3 dB Stage 3.3 KEY WIN
  reproduce under honest methodology.
- If it drops: roll back to Stage 2 and rethink.

**P2. Side-by-side benchmark vs 2Tone / fldigi / MMTTY (task #39)**

- Generate AWGN ladder of WAV files (synthetic, `rtty_gen`), run
  through:
  1. My firmware (`sweep_runner` + COM27 logger)
  2. `2Tone.exe` via Wine or native Windows, audio loopback (VAC /
     Voicemeeter)
  3. fldigi via sounddevice loopback
  4. MMTTY via sounddevice loopback
- CER per decoder per SNR.
- **Without this, all my numbers are "rumored."** Objective
  comparison is the only way to prove leadership.

**P3. Stage 5 var.2 — character N-gram LM (task #40)**

- Bigram/trigram likelihood table 32×32 (Baudot codes) or 32×32×32.
- Multiplied into Viterbi path LLR at every step.
- Corpus: ham QSO logs + English/Russian news.
- Expected gain: **+1..3 dB** — the cheapest path to −15..−16 dB.
- Implementation: Python table generator → const array in firmware →
  modify soft-Viterbi (`src/dsp_pipeline.cpp`).

**P4. Real-air dataset (task #16)**

- WebSDR and real-RX recordings: AWGN, QSB, QRM, drift scenarios.
- Format: 48 kHz / 16-bit mono.
- Needed for: (a) real-condition validation, (b) ML classifier
  training (task #23).

**P5. Stage 5 var.3 — ML post-classifier (task #23)**

- A small CNN ≤ 8K parameters, eye-diagram 16×220 → symbol.
- Training: synthetic + P4 dataset.
- Inference ~1 ms/symbol on RP2350 @ 300 MHz.
- Expected gain: +1..2 dB.

**P6. Future — IQ input (task #24)**

- Directly from SDR, bypassing the audio path and AGC/clipping.
- Requires a new hardware I/O format.
- Gain: +2..4 dB in marginal conditions.

### 9.3. Additional techniques (backlog)

- **BCJR** instead of Viterbi (+0.5..1 dB): full forward-backward MAP.
  If it fits the Core 0 budget.
- **Adaptive stop-bit soft-detector** (+0.5 dB): soft-decision
  marginal likelihood for stop = 1.0 / 1.5 / 2.0.
- **Joint optimization** of fusion weights × BW × DPLL alpha
  (+0.5..1 dB): tedious but squeezes out the last dB.

### 9.4. Execution model

- Every substep → separate build + commit + CHANGELOG + update of §7a.
- Every gain measurement → `cer_avg.py` (3-run avg, std), **with
  CR/LF in GT**.
- Every accepted change → A/B vs previous build behind a feature flag.
- Final validation — P2 benchmark vs 2Tone/fldigi/MMTTY.
