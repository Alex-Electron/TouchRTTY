# RTTY DSP — lessons learned

> 🇷🇺 [Читать на русском](RTTY_DSP_LESSONS_LEARNED.ru.md)

A record of the math, algorithm and code decisions that turned the
RP2350 into a working fldigi-class RTTY modem. The same principles
should carry over to any future digital mode (CW, FT8, etc.) on this
platform.

## 1. Absolute timing (anti-jitter)

**The problem.** Classic `sleep_us()` calls or computing deltas from
`time_us_32()` leak microseconds on every cycle. Over a second the
accumulated error shifts bit phase enough to break sync.

**The fix.** Hard absolute timing:

```cpp
uint32_t next_sample_time = time_us_32();
while (true) {
    // ... DSP math ...
    next_sample_time += 100;            // strictly +100 µs (10 kHz)
    while (time_us_32() < next_sample_time) tight_loop_contents();
}
```

Result: a rock-solid 10 000 Hz sample rate with no drift.

## 2. Core separation and flash lockout

**The problem.** When Core 1 wrote settings to internal flash, Core 0
(DSP) hard-faulted. Code execution from XIP (eXecute In Place) is
blocked by the memory controller while the sector is erased.

**The fix.**

1. All "heavy" code (FFT, DMA waterfall rendering, touch polling) is
   pinned to Core 1.
2. Before any `flash_range_erase()` I call
   `multicore_lockout_start_blocking()`. Core 1 politely asks Core 0
   to pause DSP, writes flash, releases the pause. No hard faults.

## 3. Continuous DPLL

**The problem.** On 75 baud continuous text with a hard **1.0 stop
bit**, classic decoders break. They wait for a zero-crossing to start
the next start bit, but with 1.0 stop the signal can transition
straight into Space without a clean edge.

**The fix.** A seamless framer. If I expect a 1.0 stop bit and at end
of the bit the signal is *already* in Space (negative), the state
machine immediately transitions into the start-bit-receive state and
resets `symbol_phase = 0.0f` without waiting for an edge.

## 4. PI controller in the DPLL

**The problem.** Speed mismatch. If the transmitter sends 45.45 baud
and the receiver thinks it's 45.50, phase creeps. Proportional
correction (ALPHA) couldn't cope with the accumulated static error.

**The fix.** An integrator (`freq_error`) added to the sync loop:

```cpp
symbol_phase -= ALPHA * phase_error;
freq_error -= BETA * phase_error;     // integrates clock-rate delta
// Anti-windup clamp at ±5 % of the baud rate
```

The demodulator now computes the real transmitter speed live and
adjusts its internal timer accordingly.

## 5. Digital AGC

**The problem.** Weak signals (−25 dB and below) coming off a WebSDR
have amplitude too small for the quadrature filters and squelch to
work correctly.

**The fix.** Right after the input FIR there's an AGC block with fast
attack (10 ms) and slow release (500 ms). It scales weak signals up to
a target RMS of 0.3. Gain is software-capped at ×200 (46 dB), which is
enough to pull stations off the noise floor without runaway gain on
silence.

## 6. Logarithmic squelch (dB)

**The problem.** The old linear-amplitude squelch closed on weak signal
and ghosted-open on broadband noise (random peaks).

**The fix.** All squelch math moved to dB (`20 × log10`). I introduce
a tracked noise floor (average noise). Squelch opens only when peak
station amplitude is 10–12 dB above the running noise floor.
Hysteresis: open threshold higher than close threshold, prevents
squelch chatter on fading.

## 7. Display physics and DMA (MADCTL 0x28)

**The problem.** The ILI9488 is rotated 180° in the case. Direct DMA
needed axis flipping and BGR swap. Patching coordinates in software
broke `LovyanGFX`'s calibration matrix.

**The fix.**

1. Stop hacking axes in software.
2. Hand orientation to the library: `tft.setRotation(1)` (the hardware
   equivalent of MADCTL 0x28).
3. Touch-matrix inversion goes in the `XPT2046` driver itself
   (`x_min` and `x_max` swapped at ADC read time).

The library's affine transforms now compute correct 4-point
calibration.

## 8. The LSB convention

**The problem.** RTTY is historically transmitted on F1B (LSB). In
that modulation the *lower* audio frequency is Mark (1) and the
*upper* is Space (0).

**The fix.** The demodulator is locked to the ham convention:
`Mark = CenterFreq − Shift/2`, `Space = CenterFreq + Shift/2`. An
`INV` switch flips the discriminator sign to handle USB tuning on
WebSDR.

## 9. Hardware ADC FIFO instead of software timing (Build 190)

**The problem.** Software timing via `time_us_32()` introduced
microjitter (±1–5 µs), which fed phase noise into the DPLL.

**The fix.** Switch to hardware ADC FIFO:

```cpp
adc_fifo_setup(true, false, 1, false, false);
adc_set_clkdiv(4704.0f);              // 48 MHz / (96+4704) = 10 000 Hz
adc_run(true);
// Wait via tight_loop_contents(), NOT __wfe()
while (adc_fifo_is_empty()) tight_loop_contents();
```

**Critical lesson:** `__wfe()` (Wait For Event) **cannot** be used
with ADC FIFO unless you also wire up the ADC IRQ. Otherwise the core
wakes only on unrelated events (USB SOF, ~1 ms), the FIFO overflows,
~20 % of samples are lost, and the DPLL loses phase.

## 10. FFT on Core 0 blocks the ADC (Build 190–191)

**The problem.** A 1024-point FFT on Core 0 takes ~1 ms. During that
time the ADC FIFO isn't read, overflows (depth 8), and samples are
lost. Reception breaks completely: `[ERR][ERR][ERR]...`

**The fix.** FFT lives on Core 1. Core 0 owns only the DSP pipeline
(FIR → AGC → demod → DPLL → Baudot). The rule that emerged:
**Core 0 must never run anything longer than 100 µs.**

## 11. Phosphor persistence for the eye diagram (Build 194)

**The problem.** Drawing 16 traces per frame on a real noisy signal
makes a chaotic jumpy image — you can't actually read decoder quality
from it.

**The fix.** A 2D accumulator buffer `uint8_t[240][64]`:

1. Each frame: fade `pixel = pixel * 245 / 256` (~96 % retention).
2. New traces: `pixel += 80` (clipped at 255).
3. Render: green brightness = pixel value.

Frequently-visited spots glow bright (like a real scope with
persistence), and noise spikes fade out before the next frame.

---

*Every one of these survived real-air testing — DWD weather at 50 baud
/ 450 Hz shift and amateur stations at 45.45 baud / 170 Hz shift, fed
from a PC line-out through a WebSDR.*
