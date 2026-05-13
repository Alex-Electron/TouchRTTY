# Phase 9 — progress report (B242–B245)

> 🇷🇺 [Читать на русском](PHASE9_PROGRESS_REPORT.ru.md)

> **Status: HISTORICAL.** A snapshot taken at B245 (Stages 1–2 done).
> The final result of Phase 9 is the **v2.0.0 (Build B265)** release
> with dual-IQ fusion (Stage 3), Soft-Viterbi gate, LMS notch, and the
> TinyML NN post-classifier. The current honest threshold is
> ≈ −16 dB SNR. Details in `RELEASE_v2.0.0.md` and
> `docs/ROADMAP_OPTIMIZATION.md` §8.

**Date:** 2026-04-16
**Status:** historical snapshot (B245). Final result — v2.0.0 / B265,
see banner above.

---

## 0. TL;DR

| Build | Stage | Gain (threshold) | Gain (quality) | Status |
|---|---|---|---|---|
| B230 | baseline | −10..−11 dB | baseline | — |
| B242 | 1.1 Soft-LLR | 0 dB | −14 dB alive (0 → 282 chars) | ✅ |
| B243.1 | 1.2 Soft-Viterbi | 0 dB | −8 dB 6 % → 0 % | ✅ |
| B244 | 2.1 LMS notch | 0 dB (AWGN) / +1–2 dB (QRM) | Stable under CW | ✅ |
| B245 | 2.2 Input BPF | 0 dB | −10 dB 15 % → 0 % (clean edge) | ✅ |

**Cumulative threshold gain:** 0 dB (AWGN). All the wins went into
**quality inside the range** and **QRM robustness**. The real
threshold push starts with Stage 3.

---

## 1. The honest read

Here's the catch: Stages 1–2 are **preparatory**. The plan promised
+3–5 dB from them; in practice I got ~0 dB on threshold. That's not
a failure, it's the **expected picture**:

- **Soft-LLR + Soft-Viterbi** (B242 + B243) deliver an *honest*
  framer. The decoder used to throw garbage at −8 dB (6 % CER); now it
  either stays silent or emits clean. Threshold didn't move because
  CER ≥ 5 % counts as failure, not "delivered a frame." The framer
  improved but ADC and IQ stayed the same.
- **LMS notch + BPF** (B244 + B245) are **not for AWGN**. They make
  the signal robust against QRM and interference, which AWGN tests
  don't simulate. AWGN-neutrality is the correct outcome.

The real threshold push comes from:

- **Stage 3 (fusion)**: a second IQ branch with a different ISI/SNR
  tradeoff. Weighted combine picks the better branch per symbol.
  +0.5–1.5 dB.
- **Stage 4 (spectral NR)**: FFT-based noise subtraction. +1–2 dB.
- **Stage 5 (ML post)**: a small CNN over the eye diagram. +1–2 dB.

Combined potential of Stages 3–5: **+3–5 dB** → threshold **−13..−15 dB**.
That's the target.

---

## 2. Per-stage detail — what each one actually did

### B242 — Soft-LLR bit decision

**Idea:** instead of a hard slice (sign of `integrate_acc`), keep a
soft value and validate frame boundaries with an adaptive threshold.

**Code:** `STOP_MIN_FRAC` / `START_MIN_FRAC`, normalized against the
EMA signal level (`sig_level`).

**What I got:**

- At **+20..−6 dB**: same 0–2 % (no loss).
- At **−8 dB**: false frames went UP (0 % → 6 %) — soft-LLR accepted
  weak frames whose sign was essentially random, so wrong Baudot codes
  passed as "valid."
- At **−14 dB**: the decoder **came alive** (0 → 282 characters). The
  threshold SNR where soft-LLR can hear a frame that hard-slice would
  drop.

**Verdict:** Stage 1.1 alone doesn't win anything, but it opens the
door to Stage 1.2 — now I have soft metrics I can use to filter
junk.

### B243 — Soft-Viterbi framer

**Idea:** add soft validation of **data bits** on top of the soft
border check. Two gates:

- `weakest_link`: reject if `min(|soft_data[i]|) < 0.10 · sig_level`
  — the weakest of the 5 bits has to be confident.
- `frame_avg`: reject if `mean(|soft_start| + |data[0..4]| + |stop|) / 7 < 0.15 · sig_level`
  — overall frame stats.

The first attempt (thresholds 0.20/0.30) gave a **regression**: clean
frames at +20 dB were getting cut (4.94 % CER). Relaxed to 0.10/0.15
and baseline recovered.

**What I got:**

- **−8 dB**: 6 % → 0.00 % ✓ — the B242 false frames are gone.
- **−10..−14 dB**: quality similar to B242 (weakest-link at those SNRs
  often cuts valid frames too).

**Verdict:** Stage 1.2 cleaned the **edge of the threshold**. CER=5 %
threshold stayed at −10 dB, but bin −10 stopped catching random
bytes. This is a **necessary precondition** for the next stages —
without an honest framer, any DSP improvement would catch artifacts,
not real signal.

### B244 — LMS adaptive notch

**Idea:** 2nd-order Nehorai-style constrained adaptive notch, two
instances cascaded. One scans 300–1350 Hz (below RTTY), the other
1650–3200 Hz (above RTTY). The LMS gradient `dy/da ≈ x1 − r·y1` pulls
`a` toward the dominant tone in the band; the constraint keeps the
null outside the RTTY range.

**Code:** `src/dsp/lms_notch.hpp`. Pole radius r=0.985 → BW ≈ 48 Hz.
μ=5e−6.

**What I got:**

- **AWGN-only:** threshold −10 dB **preserved**, neutral. Correct —
  AWGN has no narrowband component to null; the notch drifts a bit
  but doesn't hurt.
- **CW QRM:** threshold −10 dB, CER ≤ 2 % from +20 to −8 dB. Without
  the notch, CW usually wrecks the decoder even at +10 dB (knocks out
  AGC and IQ). Subjectively +1–2 dB.

**Caveat:** the CW level in the test wasn't fixed — needs a repeat
with a more aggressive interferer (e.g. 0 dB versus signal) to
confirm robustness.

### B245 — Input BPF 300–3000 Hz

**Idea:** Butterworth HPF@300 + LPF@3000 after AGC, before the LMS
notch. Complements the existing 63-tap FIR bandpass — kills DC/hum
< 300 Hz and HF noise > 3 kHz.

**Code:** `design_hpf()` in `src/dsp/biquad.hpp`, two biquads in
`dsp_pipeline.cpp`.

**What I got:**

- Threshold −10 dB **preserved**.
- Bin −10 dB became **0.00 %** (B243.1 had ~15 % due to merged bin).
- Clean input to LMS notch and IQ demod — ready for fusion (Stage 3).

**Verdict:** AWGN-neutral, as expected from a fixed BPF without QRM.
The main point is pipeline hygiene for the next stages.

---

## 3. Methodology notes

### 3.1 Sync markers

B241 introduced `=NN=` markers in the RTTY stream — one per SNR
point. `cer_analyze.py --markers` cuts the decoded stream by them,
giving reproducible per-SNR bins.

**Problem:** a marker can get lost (for example when LMS notch
cold-start mangles the first few characters). Then bin `N` captures
content from bin `N+1` and CER inflates artificially. In B245 I lost
the =18= marker, so bin −12 swallowed −14 and read 31 %.

**Mitigation:** keep cold-start notch < 1 s, duplicate markers (not
done yet, noted).

### 3.2 CER metric and noise

`cer_analyze.py` is Levenshtein against a cyclic GT. Optimized in
B244 (O(49·N²) → O(49·N + 3·N²)).

Sometimes shows "phantom" percentages at high SNR (7–8 %) due to
single-byte loss in serial or leftovers from a previous decode.
**Real threshold reads from a contiguous 0 % range**, not isolated
spikes.

### 3.3 AWGN vs. real-air

All measurements go through `rtty_simulator.html` → audio loopback →
ADC. That's synthetic AWGN. **Real-air** adds:

- Selective fading (frequency-selective on different shift tones)
- Impulse noise
- TX frequency drift
- QSB (slow fade)

Stages 4–5 (spectral NR + ML) may underdeliver on synthetic and
overdeliver on real-air, or vice versa. I need a **real dataset**
(task #16) before Stage 5.

---

## 4. What's next

### Stage 3 — Fusion of two IQ branches

Plan:

- **Path A** (existing): biquad LPF with BW ≈ baud.
- **Path B** (new): raised-cosine FIR with BW ≈ 1.5·baud, matched
  filter.
- **Fusion**: weighted sum of envelopes, weight = f(SNR_estimate).

Steps:

1. **3.1** — add the second IQ branch with raised-cosine FIR (no
   fusion yet, just parallel for measurement).
2. **3.2** — fusion logic, weighted combine.
3. **3.3** — SNR estimation for dynamic weights.
4. **3.4** — A/B against baseline.

Each step is its own build, commit, measurement.

### Expectations

- Stage 3: +0.5–1.5 dB (threshold −10 → −11..−11.5).
- Stage 4 (spectral NR): +1–2 dB (−11 → −12..−13).
- Stage 5 (ML): +1–2 dB (−13 → −14..−15).

Phase 9 goal: threshold −14..−15 dB. Reachable.
