# Wiring it up

A short shopping list, an even shorter pin map, and a few things to
check if the first power-up looks wrong.

## What you need

* **Raspberry Pi Pico 2 (RP2350)** — *not* the original RP2040. We
  need the M33's FPU and more SRAM than the older chip has.
* **ILI9488 480×320 SPI TFT** with resistive XPT2046 touch overlay.
  Most "3.5 inch SPI" displays on AliExpress, Amazon, etc., fit this
  description.
* An audio source — could be a PC line-out, a real radio's AF jack, a
  Bluetooth speaker fed by a phone, anything that gives you a few
  hundred mV peak-to-peak of analog audio.
* A bias network (a couple of resistors and a cap, schematic below).
* 5 V USB power for the Pico — its 3.3 V rail powers the display
  logic, but the backlight LED string typically wants 5 V.

The Pico 2 costs about $5, the display $10–15. Total bill of
materials under $25 if you scrounge passives, plus a USB cable.

---

## Pin map

### Display (ILI9488) — SPI0 + PIO

| Pico GPIO | What | ILI9488 pin |
|:---:|---|---|
| GP17 | CS  | CS |
| GP18 | SCK | SCK |
| GP19 | MOSI | SDI / MOSI |
| GP20 | DC  | DC / RS |
| GP21 | RESET | RST |
| GND  | — | GND |
| 3V3  | logic | VCC |
| 5V (VBUS) | backlight | LED + |

The display uses a PIO state machine at 60 MHz for the pixel stream
and SPI0 at 24 MHz for commands. If you got a clone that flickers or
shows garbage at 60 MHz, drop the PIO frequency in
[`src/display/ili9488_driver.h`](../src/display/ili9488_driver.h)
to 30 MHz first.

### Touch (XPT2046) — SPI1

| Pico GPIO | What | XPT2046 pin |
|:---:|---|---|
| GP10 | SCK | T_CLK |
| GP11 | MOSI | T_DIN |
| GP12 | MISO | T_DO |
| GP14 | IRQ | T_IRQ |
| GP15 | CS | T_CS |

### Audio

| Pico GPIO | What |
|:---:|---|
| GP26 (ADC0) | Audio in (1.65 V biased, line level, ≤ 1 Vpp) |

### Optional rotary encoder switch

| Pico GPIO | What |
|:---:|---|
| GP4 | Encoder switch — momentary button to GND |

* **Short press at boot** — re-run touch corner calibration only
* **Hold 3 s at boot** — full factory reset + recalibration

You don't need this — the touchscreen reset path exists from the
on-screen menu too. But it's nice to have a physical "I broke
something, factory reset" button.

---

## The audio bias network

The Pico's ADC is single-ended and reads 0–3.3 V. Your audio source
is AC (centred around 0 V). To make them play nicely you need a
DC-blocking capacitor and a divider that pulls the input pin up to
the centre of the ADC range (1.65 V).

```
audio_in ─┬─[10 kΩ]─ +3V3
          │
          ├─[10 µF tant]─ GP26 (ADC0)
          │
          └─[10 kΩ]─ GND
```

The two 10 kΩ resistors form a 1.65 V divider, the 10 µF cap blocks
the source's DC offset. Tantalum or film, whatever you have. 10 µF
gives a fairly low high-pass corner (~3 Hz) which is fine for audio.

A few notes from doing this on a breadboard a few times:

* If your source is a strong PC line-out (1 V+ swing), drop the cap
  to 1 µF. Bigger cap + bigger swing = ADC saturates on transients.
* If your source is a crystal earpiece or piezo, the source impedance
  is huge and the divider loads it down. Buffer through a single
  op-amp (TL072 etc.) first.
* For a real radio's AF-out jack: most rigs have an internal divider
  that gives you ~10–50 mV at full audio volume. That's fine — the
  Pico's ADC is happy with that, AGC will compensate.

Confirm the bias is correct by sending `STATUS` over serial after
power-up — the "ADC midpoint" reading (one of the top-row numbers
on the screen, also in `STATUS` output) should sit within ±0.05 V of
1.65 V. If it's way off, your divider resistors aren't matched well
or one isn't soldered.

---

## Feeding it audio

### Option 1: WebSDR through a computer

The easiest way to see Bohemia from your living room. We use the
University of Twente WebSDR or DWD's own one as test signals
constantly.

1. Open a WebSDR in your browser.
2. Tune to a known RTTY freq:
   * DWD weather on **4582 / 10100.8 kHz** (50 baud, 450 Hz, USB)
   * Russian commercial **4476 / 5447 / 7449 kHz** (50 baud, 170–450 Hz)
   * Amateur RTTY **7035–7040 / 14080–14100 / 21080 kHz** (45.45, 170)
3. Set the demodulator to **USB** — most RTTY traffic is tuned this
   way. If you tune LSB instead, Mark and Space swap and you'll need
   to press `INV` on the Pico.
4. Route the browser's audio to the Pico's ADC. Three ways:
   * **Voicemeeter Banana** (Windows) — set browser output to
     Voicemeeter virtual cable, route that to a physical audio output
     on your PC, run a 3.5 mm cable to the Pico bias network.
   * **Loopback cable** — straight 3.5 mm cable from PC headphone jack
     into the bias network.
   * **Air gap** — PC speaker → microphone or pickup near the Pico
     ADC. Surprisingly works, but obviously noisier.
5. On the Pico, tap **SEARCH**. It scans 300–3000 Hz and locks onto
   the strongest RTTY-looking peak. Enable **AFC** for drift tracking.

### Option 2: An actual radio

Take an AF-out / line-out / SP-out from your radio, run through the
bias network, into GP26. If your radio only has a headphone jack,
use a series resistor or a passive 2-resistor divider to drop the
signal below 1 Vpp before the bias cap.

---

## Flashing the firmware

### Pre-built `.uf2` (easiest)

Releases live in the repo root after a build-number bump (right now
that's `TouchRTTY_v2.0.0.uf2`).

Method A — BOOTSEL drag-and-drop:

1. Hold `BOOTSEL` on the Pico while plugging the USB cable in.
2. The Pico mounts as `RPI-RP2` mass storage.
3. Copy the `.uf2` onto the drive. The Pico reboots into the new
   firmware automatically.

Method B — `picotool`:

```bash
picotool load -f TouchRTTY_v2.0.0.uf2
```

### Building from source

Needs Pico SDK 2.x and an ARM toolchain. CMake-driven.

```bash
git clone --recurse-submodules https://github.com/Alex-Electron/TouchRTTY.git
cd TouchRTTY
mkdir build && cd build
cmake -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk ..
ninja
picotool load -f TouchRTTY.uf2
```

The `build/` directory is gitignored, so cmake will regenerate
everything on first run. The PIO files and LovyanGFX submodules come
along with `--recurse-submodules`.

---

## Serial console

After flashing, the Pico shows up as USB CDC ACM:

* Windows: `COM<N>` in Device Manager
* Linux: `/dev/ttyACM0`
* macOS: `/dev/tty.usbmodem*`

Open it at 115200 8N1. Either line ending works (`\r\n` or `\n`).

```bash
$ echo "VERSION" | python tools/send_serial_cmd.py --port COM27
>> TouchRTTY Phase9 B265 (built May 12 2026 05:42:11)
```

If that responds, you're good. Full command reference is in
[`SERIAL_COMMANDS.md`](SERIAL_COMMANDS.md).

---

## When the first boot looks wrong

| Symptom | Probably means |
|---|---|
| Screen is black or garbled | Check SPI wiring. Slow PIO down to 30 MHz first; clones sometimes don't tolerate 60 MHz. |
| Screen shows colours but everything is mirrored or off | Display in wrong mode. Check `src/ili9488_init.cpp` — most clones want mode 11, some need mode 0. |
| Touch dead | Check `T_IRQ` (GP14) and `T_CS` (GP15) are wired. The XPT2046 also needs the SPI1 lines on GP10/11/12. |
| Touch is mirrored or rotated | Edit `src/touch_xpt2046.cpp` — sometimes X and Y need swapping for your specific overlay orientation. |
| Top bar shows red clip indicator constantly | Audio source too loud. Drop a series resistor in front of the bias network, or turn down the source. |
| Top bar shows almost no signal | Check audio is present at GP26 (scope or oscilloscope). Or use `DUMP MS` over serial to see the mark/space envelopes. |
| ERR rate stuck near 100 % | Either wrong polarity (press `INV`) or wrong baud/shift. Try `BAUD AUTO` and `SHIFT AUTO`. |
| Constant `[ERR]` flood at low signal | Squelch too low. Drop into Tuning Lab and bump `SQ` to 8–12 dB. |
| `STATUS` says `SQ=SHUT` permanently | Squelch threshold higher than the actual SNR. Either lower SQ or improve antenna. |
| Settings vanish after every reboot | You forgot to `SAVE`. From the screen: MENU → TUNE → SAVE. |

If you really break the calibration: MENU → DIAG → RST → YES, the
Pico wipes its settings flash and reboots, and re-runs the 4-corner
touch calibration on next power-up.
