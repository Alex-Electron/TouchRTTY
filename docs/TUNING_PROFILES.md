# Tuning profiles — DSP parameter recipes

> 🇷🇺 [Читать на русском](TUNING_PROFILES.ru.md)

A short log of which DSP parameter values worked for which signals.
Numbers came out of `tools/autotune.py` running on real on-air audio,
not synthetic — these are signal-specific recipes you can dial in
directly if you want to skip the auto-tuning step.

## The three knobs

| Param | What it controls | Range |
|---|---|---|
| ALPHA | DPLL loop bandwidth (PI controller). Higher = faster lock, noisier | 0.005 – 0.200 |
| BW | LPF width factor (K). Lower = cleaner but more sensitive to drift | 0.30 – 2.00 |
| SQ | Squelch SNR threshold (dB). Higher = stricter | 1.0 – 20.0 |

---

## Profile 1 — NAVTEX 4583 kHz (50 baud, 170 Hz shift)

**Date:** 2026-04-04 (retuned)
**Source:** WebSDR → 4583 kHz USB → audio cable → ADC
**Protocol:** B 50, S 170, ST 1.5

| Param | Default | Optimized | Why |
|---|---|---|---|
| ALPHA | 0.0350 | **0.0650** | Moderately wider loop — faster adaptation to drift |
| BW | 0.75 | **1.30** | Wider filter — signal at 4583 is relatively clean |
| SQ | 6.0 | **8.0** | Stricter threshold — kills junk in the pauses |

**Result:** ERR 8 % → 6 %, SNR ~60 dB, DPLL freq error < 0.0001,
score 27 → 42.

---

## Profile 2 — DWD (German weather service) 10100.8 kHz

**Date:** 2026-04-04
**Source:** WebSDR → 10100.8 kHz USB → audio cable → ADC
**Protocol:** B 50, S 450, ST 1.5

| Param | Default | Optimized | Why |
|---|---|---|---|
| ALPHA | 0.0350 | **0.1150** | Wide loop — DWD is noisy, needs fast retracking |
| BW | 0.80 | **1.00** | Wider than NAVTEX — 450 Hz shift wants more passband |
| SQ | 9.0 | **8.0** | A touch softer — signal is weaker, hard threshold cuts useful data |

**Result:** ERR ~9 %, SNR ~52 dB, DPLL freq error < 0.0001.

---

## Profile 3 — Amateur RTTY (ham)

**Date:** —
**Protocol:** B 45, S 170, ST 1.5
**Status:** Pending — needs an on-air session to fill in.

---

## How autotune works

The autotune script (`tools/autotune.py`):

1. Connects to the device over USB serial.
2. Turns on the diagnostic stream (`DIAG ON`).
3. Sweeps each parameter across its full range (Phase 1).
4. Fine-tunes the best values at half-step resolution (Phase 2).
5. Final measurement and `SAVE`.

The scoring metric: `Score = -5×ERR + SNR - 1000×|DPLL_FE| + SQ_bonus`.

A couple of tips that make the result actually usable:

- Keep the signal stable across the full ~4 minutes of tuning. If it
  fades halfway through you'll get a tilted profile.
- Drop the `--quick` flag for serious tuning — 6 seconds per measurement
  point instead of the quick mode's shorter window.
- Run the whole thing 2–3 times. If the three runs agree, the profile
  is real. If they don't, the signal probably wasn't steady enough.
