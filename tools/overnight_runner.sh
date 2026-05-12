#!/bin/bash
set -u
cd "$(dirname "$0")/.."

PICOTOOL="/c/Users/Lavrinovich/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe"
ts() { date +%T; }

sweep_only() {
  local tag="$1"
  echo
  echo "================================================================="
  echo "$(ts) === SWEEP-ONLY: $tag ==="
  echo "================================================================="
  for s in 42 43 44; do
    local out="datasets/logs/nn_compare_${tag}_s${s}"
    rm -rf "$out"
    python -c "import sounddevice as sd; sd._terminate(); sd._initialize()" 2>/dev/null
    python tools/nn_sweep_compare.py --from -4 --to -22 --step 2 --dwell 30 \
      --center 2210 --sig-level 0.5 --seed $s --out-dir "$out" 2>&1 \
      | grep -E "SEED|per-SNR|Saved" | head -5
  done
  python tools/aggregate_compare.py datasets/logs/nn_compare_${tag}_s42 \
    datasets/logs/nn_compare_${tag}_s43 datasets/logs/nn_compare_${tag}_s44
}

train_and_sweep() {
  local tag="$1"; shift
  echo
  echo "================================================================="
  echo "$(ts) === TRAIN+SWEEP: $tag ==="
  echo "================================================================="
  echo "args: $@"
  TRAIN_NN_NOISE_MEAN=0.35 python tools/train_nn_torch.py "$@" \
    --out src/dsp/nn_weights.h 2>&1 \
    | tee "datasets/logs/train_${tag}.log" | tail -6
  cp src/dsp/nn_weights.h "datasets/nn_archive/nn_weights_${tag}.h"
  ( cd build && ninja 2>&1 | tail -2 )
  "$PICOTOOL" load -f build/TouchRTTY.uf2 2>&1 | tail -1
  sweep_only "$tag"
}

# v11 baseline (already trained, just sweep)
sweep_only "v11_baseline"

# v12 label smoothing
train_and_sweep "v12_ls005" --epochs 60 --n-synth 15000 --label-smoothing 0.05

# v13 sample weighting on uncertain frames
train_and_sweep "v13_wu3" --epochs 60 --n-synth 15000 --weight-uncertain 3.0

# v14 dropout + heavier L2
train_and_sweep "v14_drop_wd" --epochs 60 --n-synth 15000 \
  --dropout 0.1 --weight-decay 1e-3

# v15 label smoothing + sample weight (combo)
train_and_sweep "v15_ls_wu" --epochs 60 --n-synth 15000 \
  --label-smoothing 0.05 --weight-uncertain 3.0

# v16 wider arch with proper regularization
TRAIN_NN_H1=160 TRAIN_NN_H2=80 train_and_sweep "v16_wider_drop" \
  --epochs 80 --n-synth 15000 \
  --dropout 0.15 --weight-decay 5e-4 --label-smoothing 0.05

echo
echo "$(ts) === OVERNIGHT RUN COMPLETE ==="
