# Benchmarking and test tooling

Everything you'd want for measuring how well the decoder is working
lives in [`tools/`](../tools/). This doc walks through what the
individual scripts do and how they fit together. None of them require
anything beyond Python 3 + a few packages
(`numpy`, `scipy`, `sounddevice`, `pyserial`, and for the trainer
`scikit-learn` or `torch`).

The honest summary up front: most of these tools exist because we
needed to make confident claims about whether a change actually helped.
The bench infrastructure is more code than the firmware NN itself, and
that's the right ratio.

---

## Where do I start?

| Want to … | Use |
|---|---|
| Generate a synthetic RTTY WAV with controlled SNR | `tools/rtty_gen.py` |
| Play a WAV through the decoder and capture its serial output | `tools/bench_replay.py` |
| Run a full AWGN sweep and compare NN OFF vs NN ON | `tools/nn_sweep_compare.py` |
| Average N seed-runs into a mean ± σ table | `tools/aggregate_compare.py` |
| Convert a `DUMP FRAMES` log into a training-ready npz | `tools/parse_dump_frames.py` |
| Run train + flash + sweep cycles unattended | `tools/overnight_runner.sh` |
| Inspect the historical bench against 2Tone | the snapshot in `datasets/logs/bench_auto_v2/` |
| Generate test signal in a browser | open `tools/rtty_simulator.html` |

If you're just trying to verify a fresh flash works, jump to
`bench_replay.py`.

---

## Generating a known-truth WAV

`tools/rtty_gen.py` produces a stereo or mono WAV containing Baudot
RTTY with optional AWGN. The text content, baud rate, shift, centre
frequency and target SNR are all parameters. The same Baudot table is
used here, in the firmware, and in the HTML simulator — so you can
trust that what you generate is exactly what the decoder is supposed
to see.

```bash
python tools/rtty_gen.py \
    --text "RYRYRY THE QUICK BROWN FOX 1234567890" \
    --baud 45.45 --shift 170 --centre 1500 \
    --sample-rate 48000 --duration 30 \
    --snr-db -6 \
    --out test.wav
```

You usually want to set `--snr-db` to the threshold zone you care
about (−14 to −20 dB) so the WAV has the kind of difficulty you're
trying to debug. Easy SNRs decode fine on basically any version of
the firmware and don't tell you much.

---

## Playing a WAV through the decoder

`tools/bench_replay.py` plays one or more WAV files into the audio
device that the Pico's ADC listens to, and at the same time captures
everything the Pico prints over USB serial. The result is one log
file per WAV with ISO8601-timestamped serial lines plus a summary
markdown.

```bash
python tools/bench_replay.py \
    --wavs datasets/recs_mono/my_recording.wav \
    --outdir datasets/logs/realair \
    --tag run1 \
    --device "LEN Q27h-10" \
    --port COM27 \
    --gain 0.8 \
    --prep-cmd "BAUD 1" \
    --prep-cmd "SHIFT 5" \
    --prep-cmd "PATH HYB" \
    --prep-cmd "NN ON"
```

The `--tag` prefixes per-WAV log filenames so two consecutive runs
(NN OFF, then NN ON) don't overwrite each other. We learned this lesson
the hard way — losing a 4-minute capture log because we forgot to tag
the first pass is genuinely annoying.

The `--prep-cmd` flag sends serial commands to the decoder *before*
each WAV plays. Repeat it as many times as you need. Auto-centre
detection is also enabled by default: a quick spectrum scan finds the
mark/space pair in the WAV and sets the decoder's `FREQ` to the
midpoint, so the AFC doesn't have to drag itself across a big offset.
Disable with `--no-auto-center` if you want to test the AFC's tracking
range.

---

## The full sweep — `nn_sweep_compare.py`

This is the headline benchmark. It runs an SNR sweep through the
hardware decoder twice — once with NN OFF, once with NN ON — and
produces a per-SNR comparison table.

```bash
python tools/nn_sweep_compare.py \
    --from -4 --to -22 --step 2 --dwell 30 \
    --center 2210 --sig-level 0.5 \
    --seed 42 \
    --out-dir datasets/logs/nn_compare_42
```

Output `compare.txt` looks like:

```
SNR (dB)  NN OFF  NN ON  delta
        -4      19.06%      16.64%      -2.42%
        -6      13.89%      13.89%      +0.00%
        ...
```

Plus per-pass artefacts (full `cer_analyze` reports, raw serial logs)
for debugging when the CER looks off.

The script configures the decoder over serial before each pass —
sets BAUD, SHIFT, INV, PATH, NN, FREQ, AFC — so you can run it cold
without manually setting anything. Settings are restored to NN OFF at
the end, but if you `Ctrl-C` mid-run the decoder might be left in a
weird state. `STATUS` over serial tells you what's actually going on.

---

## Averaging multiple seeds

Single-run benches at low SNR are noisy. Standard deviation at
SNR ≤ −16 dB is typically 6–15 percentage points across different
random seeds, so a single run can move 20 pp up or down for the same
NN. **Anything you're about to make a production decision on should
be averaged over at least three seeds.**

```bash
for s in 42 43 44; do
    python -c "import sounddevice as sd; sd._terminate(); sd._initialize()"
    python tools/nn_sweep_compare.py \
        --from -4 --to -22 --step 2 --dwell 30 \
        --center 2210 --sig-level 0.5 --seed $s \
        --out-dir datasets/logs/sweep_s${s}
done

python tools/aggregate_compare.py \
    datasets/logs/sweep_s42 \
    datasets/logs/sweep_s43 \
    datasets/logs/sweep_s44
```

That produces a `mean ± σ` table per SNR. We standardized on this
format for committing experiments — every NN variant in our archive
has a 3-seed table next to it.

The `sd._terminate(); sd._initialize()` between seeds is a workaround
for a quirk of the LEN Q27h-10 audio device under PortAudio on Windows:
it sometimes refuses to open the second time without a full PortAudio
reset. Not strictly necessary on every machine but it's cheap to
include.

---

## The overnight chain — `overnight_runner.sh`

When we wanted to try six different training recipes overnight, we
wrote this. It's a bash script that chains `train + flash + 3-seed
sweep + aggregate` for a list of variants. Each cycle takes around
35 minutes (5 min train + 30 min audio + a few seconds analysis). Six
variants is about 3 hours unattended.

Pattern:

```bash
train_and_sweep() {
    local tag="$1"; shift
    TRAIN_NN_NOISE_MEAN=0.35 python tools/train_nn_torch.py "$@" \
        --out src/dsp/nn_weights.h
    cp src/dsp/nn_weights.h "datasets/nn_archive/nn_weights_${tag}.h"
    (cd build && ninja)
    picotool load -f build/TouchRTTY.uf2
    for s in 42 43 44; do
        python tools/nn_sweep_compare.py --seed $s \
            --out-dir "datasets/logs/nn_compare_${tag}_s${s}" \
            <other flags>
    done
    python tools/aggregate_compare.py \
        datasets/logs/nn_compare_${tag}_s42 \
        datasets/logs/nn_compare_${tag}_s43 \
        datasets/logs/nn_compare_${tag}_s44
}

train_and_sweep "v12_ls005" --epochs 60 --label-smoothing 0.05
train_and_sweep "v13_wu3"   --epochs 60 --weight-uncertain 3.0
# ... etc
```

The big benefit is you can wake up to six fully-evaluated NN variants
with archived weights and aggregated tables, ready to pick a winner
from. We did exactly this and picked v13 from the resulting evidence.

---

## DUMP FRAMES → training data

The B265 firmware addition lets you capture real-air training data
from any recording you can play through the decoder. Once you have a
serial log with `FR ...` lines in it:

```bash
python tools/parse_dump_frames.py \
    datasets/logs/dump_real_v1/wav1_*.log \
    datasets/logs/dump_real_v1/wav2_*.log \
    --out datasets/training_real.npz
```

The output is a numpy `.npz` with:

* `X`: `(N, 7)` float32, bipolar soft-bits already normalized by sig_level
* `y`: `(N,)` int32, hard-decision labels (0–31)
* `sig`: `(N,)` raw sig_level per frame
* `data_min`: `(N,)` `min(|X[:, 1:6]|) / sig` — the gate proxy

The script prints distribution statistics as it parses — label histogram
plus data_min bucket counts. You want to immediately see how many of
your captured frames are actually in the uncertain bucket, because
that's the only part of the data the NN cares about (everything above
`data_min > 0.30` is going to be gated out at inference and is
effectively dead weight for training).

---

## The CER analyzer

`tools/cer_analyze.py` does the heavy lifting of correlating a sweep
log (which tells us "between t1 and t2 the SNR was −14 dB") with a
serial log (which tells us "at time t the decoder emitted these
characters"). Output is a per-SNR-bin character error rate.

It's invoked internally by `nn_sweep_compare.py`. You can also run it
directly if you have your own sweep/serial pair:

```bash
python tools/cer_analyze.py \
    --sweep datasets/logs/my_sweep.txt \
    --serial datasets/logs/my_serial.txt \
    --gt "RYRYRY THE QUICK BROWN FOX JUMPS OVER 1234567890\r\n"
```

A quirk: the analyzer uses cyclic-rotation comparison to find the
correct alignment between the decoded text and the ground truth. For
the standard "RYRYRY..." ground truth string at 50 characters of
length, a perfect decode that happens to start on offset 14 instead of
offset 0 reads as 14 % CER from the comparison — purely an artifact
of the cyclic-rotation alignment, not actual errors. So when you see
CER numbers around 14 % on a clean decode, that's the artifact floor,
not real error rate.

When new weights show CER of e.g. 22 % on a particular SNR, subtract
the 14 % artifact baseline to estimate the real error rate (~8 pp).
For the headline comparison vs 2Tone in our memory notes, this is
what we're doing implicitly.

---

## The headline bench against 2Tone

We benchmarked TouchRTTY head-to-head against G3YYD's 2Tone 26.01a on
the same audio. The bench evidence is committed at
[`datasets/logs/bench_auto_v2/`](../datasets/logs/bench_auto_v2/) —
per-SNR `compare.txt` plus raw decoded text from both decoders. At
SNR ≤ −12 dB TouchRTTY produces readable telegraphy where 2Tone's
output is random letters.

The infrastructure that drove that bench (Win32 hwnd handshake against
N1MM Logger+, 2Tone's File→Save Text protocol, Voicemeeter routing,
etc.) was reverse-engineered for the one-off benchmark and isn't
shipped here — the goal of public-tree tooling is reproducibility for
TouchRTTY-only experiments, not full reproducibility of the 2Tone
comparison setup (which is finicky and Windows-specific). The
committed evidence stands on its own.

The TouchRTTY-only benches (`nn_sweep_compare.py` etc.) work
cross-platform.

---

## The browser RTTY generator

[`tools/rtty_simulator.html`](../tools/rtty_simulator.html) is a single
HTML file with a complete in-browser FSK generator. No build, no
Python — open it in any modern browser and you get:

* Live RTTY generation through the WebAudio API
* Configurable baud / shift / centre / SNR / ISI mixing
* Optional CR/LF insertion every N characters (matches the firmware's
  ground-truth convention)
* A WAV download button

I use it as a quick scratchpad: dial in a tough parameter combo
visually, listen to it, then either play it through the speakers
directly into a microphone-fed Pico, or download the WAV and run it
through `bench_replay.py` for measurable A/B-testing.

The Baudot ITA-2 table used here is the same one the firmware uses,
so what you see in the browser is what the decoder will see. Worth
opening it once just to play with the SNR slider and listen to what
−15 dB SNR Baudot actually sounds like — useful intuition.
