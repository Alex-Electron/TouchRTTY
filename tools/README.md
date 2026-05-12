# tools/

The Python (and one Bash) utilities for working with the decoder. Roughly
divided into three groups: bench, training, and signal generation.

If you're trying to do a specific thing, see the matching long-form doc:

* For benchmarking: [`docs/BENCH_TOOLING.md`](../docs/BENCH_TOOLING.md)
* For retraining the NN: [`docs/NN_TRAINING.md`](../docs/NN_TRAINING.md)
* For serial-port commands: [`docs/SERIAL_COMMANDS.md`](../docs/SERIAL_COMMANDS.md)

## What's here

### Signal generation

| File | What |
|---|---|
| `rtty_gen.py` | Synthetic Baudot RTTY WAV generator with optional AWGN. ITA-2 Baudot table matches the firmware exactly. |
| `sweep_runner.py` | Generates an SNR-laddered AWGN test WAV plus a sweep log for time-correlation. Called by `nn_sweep_compare.py`. |
| `rtty_simulator.html` | Single-file in-browser RTTY generator with live audio + WAV download. Useful as a scratchpad without touching the Python stack. |

### Bench and analysis

| File | What |
|---|---|
| `bench_replay.py` | Plays one or more WAVs through the audio device, captures the Pico's serial output, writes per-WAV logs and summaries. |
| `nn_sweep_compare.py` | The headline benchmark — runs an SNR sweep twice (NN OFF, NN ON) on hardware and produces a per-SNR comparison table. |
| `cer_analyze.py` | Workhorse CER analyzer. Correlates a sweep log against a serial-decode log against ground truth. Used internally by `nn_sweep_compare.py`. |
| `aggregate_compare.py` | Reduces N seed-runs into a mean ± σ table. Single-run benches at low SNR are too noisy to trust on their own. |
| `parse_dump_frames.py` | Converts the B265 `DUMP FRAMES` serial output into a numpy npz of training samples. |
| `overnight_runner.sh` | Bash chain that runs `train + flash + 3-seed sweep + aggregate` for a list of NN variants. Used to evaluate many recipes unattended. |

### NN training

| File | What |
|---|---|
| `train_nn_torch.py` | The PyTorch trainer that produced the v13 production weights. Supports per-sample loss weighting, label smoothing, dropout, real-air mixing. |
| `train_nn.py` | Legacy sklearn `MLPClassifier` trainer. Kept because it owns the `generate_synthetic` data function used by both trainers. |

### Serial

| File | What |
|---|---|
| `send_serial_cmd.py` | Send one or more CRLF-terminated commands to the decoder over USB. |

## A typical workflow

The simplest reproducible bench — run an AWGN sweep on a real Pico,
compare NN OFF vs NN ON:

```bash
python tools/nn_sweep_compare.py \
    --from -4 --to -22 --step 2 --dwell 30 \
    --center 2210 --sig-level 0.5 --seed 42 \
    --out-dir datasets/logs/my_sweep
```

For production decisions, run three seeds and average:

```bash
for s in 42 43 44; do
    python tools/nn_sweep_compare.py --seed $s \
        --out-dir datasets/logs/my_sweep_s${s} <same flags>
done
python tools/aggregate_compare.py datasets/logs/my_sweep_s{42,43,44}
```

For real-air audio (not synthesised) instead of an AWGN ladder, use
`bench_replay.py` against a recorded WAV — same serial-log + analyzer
pattern.

For retraining the NN, the v13 recipe is one line:

```bash
TRAIN_NN_NOISE_MEAN=0.35 python tools/train_nn_torch.py \
    --epochs 60 --n-synth 15000 --weight-uncertain 3.0
```

Then build + flash + bench. The `overnight_runner.sh` chain automates
all of that for multiple variants.
