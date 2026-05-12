# Using the touchscreen

The 480×320 ILI9488 panel has resistive touch and shows three persistent
zones plus a few overlay screens. Once you spend ten minutes with it,
you don't really need to look at this doc again — but here it is for
the first ten minutes.

The big picture: top bar shows live metrics, the middle splits between
the visualizer and the decoded RTTY text, and the bottom has eight
quick-access buttons. Everything else (menu, tuning lab, diagnostics)
overlays the middle area when you tap into it.

```
+----------------------------------------------------+
|  TOP BAR (33 px) — SIG / AGC / ERR + metrics       |
+----------------------------------------------------+
|                                                    |
|           MAIN ZONE  (160 px)                      |
|           Waterfall / spectrum / scope             |
|                                                    |
+----------------------------------------------------+
|       TEXT ZONE (160 px) — decoded RTTY text       |
+----------------------------------------------------+
|  BOTTOM BAR (8 buttons × 1 row)                    |
+----------------------------------------------------+
```

---

## The bottom bar

These eight buttons are always there. Tap to cycle / toggle.

**1. Baud rate.** Cycles `B 45 → B 50 → B 75 → B 100 → B:AUTO`. The
auto detector is good — when you don't know, leave it on AUTO.

**2. Shift.** Cycles through 85, 170, 200, 340, 425, 450, 500, 850 Hz,
then AUTO. For ham radio it's 170. For DWD weather it's 450. Outside
those two cases, AUTO usually finds it within a few seconds.

**3. Stop bits.** 1.0 / 1.5 / 2.0 / AUTO. 1.5 is the default and the
right answer 95 % of the time. The auto-detector watches gap statistics
and picks the most likely.

**4. Polarity.** `NOR` / `INV` / `NOR[A]` / `INV[A]`. The `[A]` suffix
means we're in auto mode — the decoder will flip on its own if errors
stay too high for too long. Without `[A]` you've manually locked it.
Border colour: cyan = auto, magenta = manual lock.

**5. AFC.** `AFC:ON` / `AFC:OFF`. Tracks frequency drift up to ±100 Hz
from the configured `FREQ`. Almost always you want this on. Off only
makes sense if you're doing something weird like measuring drift
yourself.

**6. SEARCH.** Tap to run an FFT scan across 300–3000 Hz and land on
the strongest RTTY-looking peak. While searching the button reads
`SRCH..` (yellow). When it finishes it briefly shows `FOUND!` (green)
or `NONE` (red) before going back to `SEARCH`.

**7. CLEAR.** Resets DSP integrators — AGC gain, DPLL phase, the
error-rate sliding window. Doesn't change any settings. Useful if AGC
got pumped up by a static crash and won't come back down.

**8. MENU.** Toggles the menu overlay. Tap once to open, again to
close.

Buttons highlight green when their parameter is currently ON.

---

## The top bar

Three rows in 33 pixels of vertical real-estate, each row a bar plus
some numbers.

**Row 1 — SIG.** Horizontal bar showing absolute signal level
(−80 to −10 dB FS). To the right: SNR in dB, raw signal dB, the two
mark/space frequencies the decoder thinks it's locked onto, AFC
offset from the configured FREQ.

**Row 2 — AGC.** Bar shows current AGC gain (0–40 dB). Numbers next
to it: AGC ×factor, CPU loads for both cores, frames-per-second of
the UI, ADC midpoint voltage (should sit near 1.65 V; way off means
the bias network needs adjustment).

**Row 3 — ERR.** Bar shows rolling error rate over the last
100 frames (0–100 %). Numbers: effective baud and shift the decoder
is locked on, plus little icons showing NN / NOTCH / VIT state and a
clipping flag if the ADC is saturating.

Off to the right edge there are tiny indicators for FIGS / LTRS
state, polarity-uncertain hint, and squelch state. Not critical for
day-to-day use.

---

## The menu overlay

Tap `MENU` (button 8 on the bottom bar) and the text zone is
replaced by a 4×3 grid. Tap a slot to act, tap `MENU` again to close.

The grid layout, top-left to bottom-right:

* **DISP: WF / SPEC / SCOPE** — cycle the visualizer mode.
* **DIAG** — open the diagnostics screen.
* **TUNE** — open the Tuning Lab.
* **PATH: A / B / HYB / HYB+NN** — cycle the decoder path. Four
  states, not three: the fourth one is "HYB with NN classifier
  enabled". Cycling here is the same as `PATH HYB; NN ON` over serial.
* **NOTCH: ON / OFF** — toggle the LMS notch chain.
* **VIT: ON / OFF** — toggle the soft-Viterbi frame gate.
* (six empty slots reserved for future use)

You'll notice this is more compact than older builds, where NOTCH and
VIT used to require a popup. We collapsed them into menu slots
4 and 5 — fewer taps, easier to mentally model.

---

## The Tuning Lab

This is where you actually tweak the DSP. Open it from `MENU` → `TUNE`.
You get the eye diagram on top, and a 6×2 button grid below.

The eye diagram is the most useful real-time feedback you have. It
overlays consecutive bit traces with phosphor persistence (~96 %
retention per frame). Clean eye = DPLL is locking. Smeared eye = bump
ALPHA. Wrong eye crossings = wrong bit timing.

The button grid is six columns by two rows:

Row 0:

| `A-` | A val | `A+` | `K-` | K val | `K+` |
|---|---|---|---|---|---|

* `A` is the DPLL alpha (loop bandwidth). −/+ steps by 0.005, range
  0.005–0.200.
* `K` is the LPF bandwidth factor. −/+ steps by 0.05, range 0.3–2.0.

Row 1:

| `SQ-` | SQ val | `SQ+` | `DUMP` | (empty) | `SAVE` |
|---|---|---|---|---|---|

* `SQ` is the squelch SNR threshold. ±1 dB steps.
* `DUMP` toggles the periodic diagnostic stream over serial. Same as
  `DIAG ON/OFF` from a serial console.
* `SAVE` persists *everything* (settings, calibration) to flash.

To leave the Tuning Lab tap `MENU` again. The eye diagram is also
visible briefly if you go back through the menu, so don't worry about
losing your view.

---

## The DIAG screen

From `MENU` → `DIAG`. Shows a character histogram (how often each
ITA-2 code has been decoded recently), and a zero-bias meter — a
horizontal indicator showing how far off-centre the ADC bias is from
the ideal 1.65 V midpoint. Green when within ±50 mV, blue when farther
off. Useful for confirming your bias network is well-trimmed.

Two action buttons at the bottom:

* **FONT: BIG / MED / SML / TINY** — cycle through the four
  on-screen font sizes. The line width adjusts automatically: 55
  chars (BIG), 62 (MED), 73 (SML), 90 (TINY).
* **RST** — opens the reset-confirm dialog.

---

## The reset-confirm dialog

Tap `RST` and you get a full-screen modal: big YES on the right, NO on
the left.

* **YES** wipes the entire settings flash page and triggers a
  watchdog reboot.
* **NO** returns to the DIAG screen, no harm done.

The modal eats stray taps anywhere else, so you can't accidentally
dismiss it. But once you tap YES the reset is unrecoverable — your
DPLL alpha tweaks, AFC habit, touch calibration, everything is gone.
Don't tap it idly.

After a reset the first boot will show a 4-corner calibration
overlay — tap each corner crosshair, then `SAVE` from the Tuning Lab
when everything reads right.

---

## Visualizer modes

The DISP slot cycles three modes:

* **WF** (waterfall) — vertical scrolling spectrogram, the default.
  Time goes down (about 7 pixels per second), frequency across. Easy to
  spot RTTY's characteristic two-tone signature.
* **SPEC** (spectrum analyzer) — magnitude versus frequency, with
  peak markers and mark/space cursors painted on top. Useful when
  you want to see *now* rather than the history.
* **SCOPE** (Lissajous / XY) — plots I against Q. For clean RTTY you
  see two clusters in the bipolar pattern. FT8 / PSK look completely
  different here, which is sometimes a useful diagnostic.

---

## What the on-screen text actually shows

| Character | Meaning |
|---|---|
| `A`-`Z`, `0`-`9`, punctuation | A successfully decoded character |
| `[LTRS]` / `[FIGS]` | Baudot shift markers. Shown in serial; hidden in normal screen rendering. |
| `*` (red) | A frame that failed validation (B243 / B264 / sign-threshold). The full `[ERR]` token goes to the serial port. |

The red `*` is a B263 addition — older firmware showed `[ERR]` on the
screen too, which cluttered the line and made the actual readable text
hard to follow. Single character keeps the line density readable.

If you're trying to count errors precisely, use the ERR percentage in
the top bar (sliding window over the last 100 frames) — that's the
metric. Counting `*` glyphs visually is unreliable on a fast scrolling
text zone.

---

## When you don't know what to tap

| What you want | What to do |
|---|---|
| Decode a brand-new band, no idea what's there | Tap `SEARCH`. Wait. |
| Switch from amateur RTTY to DWD weather | Bottom bar: `B 45` → `B 50`, then `S 170` → `S 450`. Polarity stays NOR. |
| Errors are pouring in on a clean-looking signal | Try `INV`. If that doesn't help, drop into Tuning Lab and ease `K` up a bit (wider LPF). |
| The NN is making things worse | Set PATH to HYB (without +NN). Or `NN OFF` over serial. |
| Lose all your tuning to a bad afternoon | `MENU` → `DIAG` → `RST` → YES. Start clean. |
| Keep your tuning between reboots | `MENU` → `TUNE` → `SAVE`. |
