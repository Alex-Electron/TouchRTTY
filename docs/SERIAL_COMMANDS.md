# Driving TouchRTTY over USB serial

> 🇷🇺 [Читать на русском](SERIAL_COMMANDS.ru.md)

Once the Pico is flashed and powered up, it shows up as a USB CDC ACM
device — `COM<N>` on Windows, `/dev/ttyACM0` on Linux,
`/dev/tty.usbmodem*` on macOS. Open it at **115200 8N1**, send `VERSION`,
and you should get something like

```
>> TouchRTTY Phase9 B265 (built May 12 2026 05:42:11)
```

If you get that, the serial path is alive and I can do anything.
If you don't, check that you're on the right port and that no other
program (PuTTY, PyCharm terminal, Arduino IDE) is holding it open.

A command is a single line. The Pico accepts both `\r\n` and bare `\n`
as line endings. There's also a 500 ms idle timeout — if you type
something and pause, the box will execute it after half a second
regardless. Useful if you're talking to it from a script that doesn't
explicitly send a newline.

Typing `HELP` on the box gives a terse summary. This doc is the long
form with examples and the small surprises that aren't obvious from
the help screen.

---

## Tuning the DSP

These four numbers are the heart of the loop. Changing them takes effect
on the next sample — the decoder doesn't restart. If you find a sweet
spot, `SAVE` writes them to flash so they survive a reboot.

| Command | Range | Default | What it does |
|---|---|---|---|
| `ALPHA <0.005..0.200>` | float | 0.05 | DPLL loop bandwidth. Higher = locks fast, jitters more. Lower = clean timing but slow to grab a new signal. |
| `BW <0.3..2.0>` | float | 1.0 | LPF `K` factor. Wider lets more frequency offset through; narrower rejects noise but is fussier about being on-frequency. |
| `SQ <dB>` | float | 6.0 | Squelch threshold. Below this SNR, output silences. Bump it up if you're getting `[ERR]` storms from pure noise. |
| `FREQ <Hz>` | float | 1500 | Audio centre frequency the decoder listens on. If AFC is enabled, this is just a starting hint. |

```
> ALPHA 0.07
>> ALPHA=0.0700

> BW 1.4
>> BW=1.40

> FREQ 2210
>> FREQ=2210.0
```

`BW` and `K` are aliases for the same setting, in case you've seen it
called either in older docs.

---

## Telling it what kind of RTTY to expect

You have to set baud, shift, stop bits, and polarity. The defaults
are amateur RTTY (45.45 / 170 / 1.5 / NOR), which works for most ham
band activity but fails on commercial stations.

| Command | What you're picking |
|---|---|
| `BAUD 0` / `BAUD 1` / `BAUD 2` / `BAUD 3` | 45.45 / 50 / 75 / 100 baud |
| `BAUD AUTO` (or `BAUD 4`) | Let it figure out the baud rate |
| `SHIFT 0..7` | 85 / 170 / 200 / 340 / 425 / 450 / 500 / 850 Hz spacing |
| `SHIFT AUTO` (or `SHIFT 8`) | Auto-detect shift |
| `STOP 0..2` | 1.0 / 1.5 / 2.0 stop bits |
| `STOP AUTO` (or `STOP 3`) | Auto-detect from frame gap stats |
| `INV NOR` / `INV INV` | Polarity. NOR = Mark is the high tone. Most amateur. |
| `INV AUTO` | Flip if errors stay high for ~0.8 s |

The auto modes are good — `AUTO` for everything works most of the time.
The main reason to lock them manually is if you know exactly what you're
listening to and want to skip the detection time.

Polarity is the one that bites people. If you tune the WebSDR in USB
and the station is actually transmitting LSB (DWD does), Mark and Space
swap and you'll see garbage. Tap `INV` once. Done.

**Useful combos I keep on hand:**

| Signal | Settings |
|---|---|
| Amateur RTTY, any band | `BAUD 0` `SHIFT 1` `INV NOR` |
| DWD weather (4582 / 10100.8 kHz, USB-tuned) | `BAUD 1` `SHIFT 5` `INV NOR` |
| Russian commercial / Slavyanka | `BAUD 1` `SHIFT 1` `INV INV` often |
| SITOR-B / NAVTEX | `BAUD 2` `SHIFT 1` |

---

## On/off switches

Quick toggles. Each prints back what it just did.

* `AFC ON` / `AFC OFF` — automatic frequency tracking. Drifts up to
  ±100 Hz from the configured `FREQ`. Always-on for me.
* `AGC ON` / `AGC OFF` — automatic gain. Turn off if you want absolute
  amplitudes for the SNR meter to mean something physical.
* `SCALE EXP` / `SCALE LIN` — waterfall colour mapping. `EXP` lifts
  weak signals out of the background, makes everything look more
  alive but a touch noisier.
* `WIDTH <30..120>` — characters per on-screen line. Adjust to taste
  vs. font size.
* `SEARCH` — forces an FFT scan of 300–3000 Hz, picks the strongest
  RTTY-looking carrier, sets FREQ and turns AFC on. Same as tapping
  the SEARCH button on the bottom bar.

---

## Picking a decoder path

There are two parallel demodulator chains: a narrow filter (path A,
good on weak clean signals) and a wide one (path B, good on fading or
unknown-shift signals). You can run either alone or fuse them.

* `PATH A` — narrow chain only
* `PATH B` — wide chain only
* `PATH HYB` (or `PATH LLR`) — log-likelihood-ratio fusion of both.
  This is what I run by default and it's almost always the answer.
* `DYN ON` / `DYN OFF` — controls how the HYB fusion weights the two
  chains. ON = weighted by per-chain SNR estimate. OFF = pure
  geometric mean. Default is ON, but OFF can be more robust if one
  chain estimates its SNR badly.
* `WEIGHTS <wa> <wb>` — manual weights if you want to force the
  fusion ratio. Internally normalized to sum to 1, so
  `WEIGHTS 7 3` is the same as `WEIGHTS 0.7 0.3`.

I haven't found a case where PATH B alone beats HYB. Path A wins
occasionally on extremely clean weak amateur signals. Mostly HYB.

---

## The neural net classifier

Since B261 there's a small (~44 KB) MLP that gets a vote on
ambiguous Baudot frames. Two commands turn it on and off:

* `NN ON` — engage the classifier
* `NN OFF` — pure sign-threshold decision, no NN

Crucially, even with `NN ON`, the network only fires when input is
genuinely uncertain — specifically when the weakest data-bit's
absolute value is below 30 % of the estimated signal level
(the B264 confidence gate). If your bits are crisp, the hard
decision is already correct and the NN bypasses itself. This is what
keeps the NN from making things *worse* on clean signals — an
earlier version of the integration didn't do this and it created a
nasty U-shaped performance curve. See
[NN_TRAINING.md](NN_TRAINING.md) for the story.

Production weights are **v13** (PyTorch, `weight_uncertain=3.0`
recipe). If you want to roll your own, that doc walks through it.

---

## Filtering and frame validation

* `NOTCH ON` / `NOTCH OFF` — LMS adaptive notch chain. Kills narrow
  carriers / heterodynes inside the audio passband. Worth turning on
  when there's a stray BFO leak or another station nearby.
* `VIT ON` / `VIT OFF` — soft-Viterbi frame gate. ON = strict (full
  energy + start/stop validation). OFF = stop-bit-only (more
  permissive, lets more text through but with more errors).
* `NR ON` / `NR OFF` — Wiener spectral noise reduction. Experimental.
  I tried it three runs in a row and it didn't help measurably, so
  default OFF. Kept the toggle in case you want to test it on weird
  noise.

---

## Persistence and inspection

* `SAVE` — write all current settings to the flash settings page.
  They'll come back on next boot. Touch calibration too.
* `CLEAR` — reset DSP integrators (AGC gain, DPLL phase, error
  history). Doesn't change any settings, just clears the runtime
  state. Useful if AGC is stuck high and won't recover.
* `STATUS` — dump everything. This is your friend when you're
  troubleshooting:

```
> STATUS

=== STATUS (B265) ===
ALPHA=0.0500 BW=1.00 SQ=6.0
BAUD=50 SHIFT=5(450) INV=NOR AFC=ON
FREQ=2210.0 SNR=14.2 SIG=-12.3 AGC=1.50
STOP=1.5(1.5) SQ=OPEN ERR=3% DIAG=OFF
STOP-DET: gap_last=1.48T hist[1.0/1.5/2.0]=2/85/13
NN=ON NOTCH=OFF VIT=ON
====================
```

* `VERSION` (aliases `VER`, `ID`) — firmware build number and the
  C++ compilation timestamp. Worth grabbing if you're filing a bug.

---

## Diagnostic streams

For deep debugging:

* `DIAG ON` / `DIAG OFF` — turn on a periodic ~500 ms condensed
  diagnostic line. Useful if you want to see SNR and AGC moving in
  real time without touching the screen.
* `DUMP SPEC` — one-shot dump of the current 512-bin FFT magnitudes.
  Useful for offline spectrum analysis.
* `DUMP MS` — one-shot dump of 480 samples of Mark and Space
  envelopes. Useful for inspecting the demodulator output.

### `DUMP FRAMES` — the training-data capture

This is the B265 addition that lets you bootstrap NN training from
real-air recordings.

```
> DUMP FRAMES ON
>> DUMP FRAMES ON
```

After this, every Baudot frame the decoder accepts produces one line:

```
FR -35.50 184.20 -176.40 -188.10 192.30 156.70 -178.80 175.10 16
   ^      ^----------- 5 data bits ----------- ^      ^   ^
   start                                       stop   sig hard_char
```

Values aren't normalized. Divide everything except `sig` and
`hard_char` by `sig` to get the bipolar ±1 representation the NN
expects. The `hard_char` value (0–31) is the pure sign-threshold
decision *before* any NN override, so it works as a label for
training data when you're sure the decoder got it right.

Turn it off with `DUMP FRAMES OFF`. The companion tool
[`tools/parse_dump_frames.py`](../tools/parse_dump_frames.py) reads a
serial log and writes a numpy `.npz` ready for the trainer.

---

## Quick recipes

### Tune to DWD on 10100.8 kHz USB

```
BAUD 1
SHIFT 5
INV NOR
FREQ 1700      # depends on the WebSDR's audio mapping, adjust
AFC ON
PATH HYB
NN ON
```

### A/B test the NN on the same audio

```
NN OFF
# play your test WAV through audio loopback, log serial
NN ON
# play the same WAV again, log serial again
# diff the two logs / run cer_analyze on both
```

### Capture training data from a known-good recording

```
NN OFF              # I want hard-decision labels
DUMP FRAMES ON
# play the recording, capture serial to file
DUMP FRAMES OFF
```

Then push the log through `parse_dump_frames.py` and you've got an
npz to feed `train_nn_torch.py --real-npz ...`.

---

## When something doesn't match

Anything unrecognised gets:

```
>> UNKNOWN: <whatever you typed> (try HELP)
```

I sometimes use this as a low-effort ping — if `PING` comes back as
`>> UNKNOWN: PING` then I know the serial path is alive.

If you sent a valid command and it's silent, check:
1. Did the newline make it? Some serial terminals send only `\r`.
2. Is another process holding the port?
3. Did the Pico crash? `VERSION` should always answer. If it doesn't,
   power-cycle.
