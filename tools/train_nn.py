#!/usr/bin/env python3
"""train_nn.py — Train TinyML MLP for RTTY Baudot character recognition.

Input features: 7 soft-bit values per frame [start, d0..d4, stop]
  +1.0 = strong MARK, -1.0 = strong SPACE, values in between = uncertain

Architecture: Input(7) -> Dense(128, ReLU) -> Dense(64, ReLU) -> Dense(32, Linear)

Output: src/dsp/nn_weights.h with C++ float arrays ready for embedded inference.

Usage:
    python tools/train_nn.py                             # synthetic only
    python tools/train_nn.py --csv path/to/data.csv      # synthetic + real 7-col data
    python tools/train_nn.py --ultra path/to/ultra.csv   # + real 21-col data (collapsed)
"""
from __future__ import annotations
import argparse, os, sys
import numpy as np
from pathlib import Path

try:
    from sklearn.neural_network import MLPClassifier
    from sklearn.model_selection import train_test_split
    from sklearn.utils import shuffle as sk_shuffle
except ImportError:
    sys.exit("sklearn required: pip install scikit-learn")

HEADER_PATH = Path("src/dsp/nn_weights.h")
HIDDEN1 = 128
HIDDEN2 = 64
# Override via env if you want a wider net without editing this file:
# TRAIN_NN_H1, TRAIN_NN_H2
HIDDEN1 = int(os.environ.get("TRAIN_NN_H1", HIDDEN1))
HIDDEN2 = int(os.environ.get("TRAIN_NN_H2", HIDDEN2))
N_CLASSES = 32


def make_ideal_frame(char_code: int) -> np.ndarray:
    """Ideal bipolar soft frame for Baudot code char_code (0-31)."""
    bits = [(char_code >> i) & 1 for i in range(5)]
    frame = [-1.0] + [1.0 if b else -1.0 for b in bits] + [1.0]
    return np.array(frame, dtype=np.float32)


def generate_synthetic(n_per_char: int = 15000) -> tuple[np.ndarray, np.ndarray]:
    """Generate training frames with SNR-distributed AWGN + ISI.

    Noise sigma drawn from Exp(mean=NOISE_MEAN) clipped to [0.04, 1.1].
    Default 0.28 gives ~50% frames with sigma<0.28 (high SNR) and a tail.
    Override via TRAIN_NN_NOISE_MEAN env var; e.g. 0.35 shifts more weight
    to threshold-zone (low-SNR) samples so the NN learns to disambiguate
    soft-bits where it actually matters.
    """
    noise_mean = float(os.environ.get("TRAIN_NN_NOISE_MEAN", 0.28))
    # Gate-aware training: keep only samples whose min |data_bit| / snr_scale
    # falls below TRAIN_NN_GATE_FILTER (0=disabled). Matches the B264 inference
    # gate that only runs NN when data_min < GATE_FRAC * sig_level.
    gate_filter = float(os.environ.get("TRAIN_NN_GATE_FILTER", 0.0))
    rng = np.random.default_rng(42)
    total = n_per_char * 32
    print(f"[*] Generating {total} synthetic frames "
          f"(exp-noise mean={noise_mean}"
          + (f", gate_filter={gate_filter}" if gate_filter > 0 else "")
          + ")...")

    # Pre-generate all sigma values vectorised
    sigmas = rng.exponential(noise_mean, total)
    sigmas = np.clip(sigmas, 0.04, 1.10).astype(np.float32)
    isi_alphas = rng.uniform(0.04, 0.32, total).astype(np.float32)
    snr_scales = rng.uniform(0.35, 2.2, total).astype(np.float32)

    X = np.empty((total, 7), dtype=np.float32)
    y = np.empty(total, dtype=np.int32)

    idx = 0
    for c in range(32):
        ideal = make_ideal_frame(c)
        for _ in range(n_per_char):
            noise = rng.normal(0, sigmas[idx], 7).astype(np.float32)
            s = ideal + noise
            alpha = isi_alphas[idx]
            isi = s.copy()
            for i in range(1, 7):
                isi[i] = s[i] * (1.0 - alpha) + s[i - 1] * alpha
            X[idx] = isi * snr_scales[idx]
            y[idx] = c
            idx += 1

    if gate_filter > 0:
        # data_min = min |X[i, 1:6]| (5 data bits); normalize by signal scale
        data_min = np.min(np.abs(X[:, 1:6]), axis=1) / snr_scales
        keep = data_min < gate_filter
        kept = int(keep.sum())
        print(f"[*] Gate-filter ({gate_filter}): kept {kept}/{total} "
              f"({100*kept/total:.1f}%) — uncertain frames only")
        X = X[keep]
        y = y[keep]

    return X, y


def load_csv_7(path: str) -> tuple[np.ndarray, np.ndarray]:
    """Load 7-feature real-captured CSV (label,f0..f6 or f0..f6,label)."""
    import pandas as pd
    df = pd.read_csv(path)
    if 'label' in df.columns:
        yv = df['label'].values.astype(np.int32)
        Xv = df.drop('label', axis=1).values.astype(np.float32)
    else:
        yv = df.iloc[:, -1].values.astype(np.int32)
        Xv = df.iloc[:, :-1].values.astype(np.float32)
    if Xv.shape[1] != 7:
        print(f"[warn] CSV has {Xv.shape[1]} features, expected 7 — skipping")
        return np.zeros((0, 7), np.float32), np.zeros(0, np.int32)
    print(f"[+] Loaded {len(Xv)} frames from {path}")
    return Xv, yv


def load_ultra_csv(path: str) -> tuple[np.ndarray, np.ndarray]:
    """Load 21-feature ultra-dataset and collapse 3-sample triplets to 7 means.

    Neighboring project collects 3 sub-samples per bit position (7 positions).
    Averaging each triplet gives us exactly the soft-bit representation our
    7-input model expects, while using all 100K+ real hardware captures.
    """
    import pandas as pd
    print(f"[*] Loading ultra dataset from {path} ...")
    df = pd.read_csv(path)
    if 'label' not in df.columns or df.shape[1] != 22:
        print(f"[warn] Ultra CSV not in expected format (22 cols incl label) — skipping")
        return np.zeros((0, 7), np.float32), np.zeros(0, np.int32)
    yv = df['label'].values.astype(np.int32)
    raw = df.drop('label', axis=1).values.astype(np.float32)  # (N, 21)
    # Average triplets: col[0..2] -> pos0 (START), col[3..5] -> pos1 (d0), ...
    collapsed = raw.reshape(-1, 7, 3).mean(axis=2)  # (N, 7)
    print(f"[+] Collapsed {len(collapsed)} ultra frames (21->7) from {path}")
    return collapsed, yv


def format_array_c(name: str, arr: np.ndarray) -> str:
    flat = arr.flatten().astype(np.float32)
    vals = ", ".join(f"{v:.6f}f" for v in flat)
    return f"const float {name}[{len(flat)}] = {{\n    {vals}\n}};\n"


def export_weights(w1, b1, w2, b2, w3, b3) -> None:
    HEADER_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(HEADER_PATH, "w", encoding="utf-8") as f:
        f.write("#ifndef NN_WEIGHTS_H\n#define NN_WEIGHTS_H\n\n")
        f.write("// Auto-generated TinyML weights — DO NOT EDIT MANUALLY\n")
        f.write(f"// Architecture: Input(7) -> Dense({HIDDEN1}, ReLU)"
                f" -> Dense({HIDDEN2}, ReLU) -> Dense({N_CLASSES}, Linear)\n\n")
        f.write(f"#define NN_INPUT  7\n")
        f.write(f"#define NN_H1     {HIDDEN1}\n")
        f.write(f"#define NN_H2     {HIDDEN2}\n")
        f.write(f"#define NN_OUT    {N_CLASSES}\n\n")
        f.write(format_array_c("nn_w1", w1))
        f.write(format_array_c("nn_b1", b1))
        f.write(format_array_c("nn_w2", w2))
        f.write(format_array_c("nn_b2", b2))
        f.write(format_array_c("nn_w3", w3))
        f.write(format_array_c("nn_b3", b3))
        f.write("\n#endif // NN_WEIGHTS_H\n")
    print(f"[+] Weights saved to {HEADER_PATH}")
    total = (7*HIDDEN1 + HIDDEN1 + HIDDEN1*HIDDEN2 + HIDDEN2 + HIDDEN2*32 + 32)
    print(f"[+] Total parameters: {total} floats = {total*4/1024:.1f} KB")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv",   help="Optional real 7-col CSV (label,f0..f6)")
    ap.add_argument("--ultra", help="Optional real 21-col ultra CSV (f0..f20,label)")
    ap.add_argument("--real-npz", action="append", default=[],
                    help="Real-air npz from parse_dump_frames.py (X, y); repeatable")
    ap.add_argument("--real-replicate", type=int, default=1,
                    help="Replicate real samples N times before mixing (default 1)")
    ap.add_argument("--n-synth", type=int, default=15000,
                    help="Synthetic samples per char (default 15000 = 480K total)")
    args = ap.parse_args()

    X, y = generate_synthetic(n_per_char=args.n_synth)

    for npz_path in args.real_npz:
        if Path(npz_path).exists():
            d = np.load(npz_path)
            Xr, yr = d['X'].astype(np.float32), d['y'].astype(np.int32)
            if args.real_replicate > 1:
                Xr = np.tile(Xr, (args.real_replicate, 1))
                yr = np.tile(yr, args.real_replicate)
            print(f"[+] Loaded {len(Xr)} real-air frames from {npz_path}"
                  + (f" (x{args.real_replicate})" if args.real_replicate > 1 else ""))
            X = np.vstack([X, Xr])
            y = np.concatenate([y, yr])

    if args.ultra and Path(args.ultra).exists():
        Xu, yu = load_ultra_csv(args.ultra)
        if len(Xu):
            X = np.vstack([X, Xu])
            y = np.concatenate([y, yu])

    if args.csv and Path(args.csv).exists():
        Xr, yr = load_csv_7(args.csv)
        if len(Xr):
            X = np.vstack([X, Xr])
            y = np.concatenate([y, yr])

    X, y = sk_shuffle(X, y, random_state=42)
    X_tr, X_val, y_tr, y_val = train_test_split(X, y, test_size=0.1, random_state=42)
    print(f"[*] Train={len(X_tr)}, Val={len(X_val)}")

    print(f"[*] Training MLP (7 -> {HIDDEN1} -> {HIDDEN2} -> 32)...")
    mlp = MLPClassifier(
        hidden_layer_sizes=(HIDDEN1, HIDDEN2),
        activation="relu",
        solver="adam",
        alpha=0.0003,
        learning_rate_init=0.001,
        batch_size=1024,
        max_iter=600,
        random_state=42,
        verbose=False,
        early_stopping=True,
        validation_fraction=0.05,
        n_iter_no_change=25,
    )
    mlp.fit(X_tr, y_tr)

    val_acc = mlp.score(X_val, y_val) * 100
    print(f"[+] Validation accuracy: {val_acc:.2f}%")

    w1 = mlp.coefs_[0]
    b1 = mlp.intercepts_[0]
    w2 = mlp.coefs_[1]
    b2 = mlp.intercepts_[1]
    w3_raw = mlp.coefs_[2]
    b3_raw = mlp.intercepts_[2]

    w3 = np.zeros((HIDDEN2, 32), dtype=np.float32)
    b3 = np.full(32, -1e9, dtype=np.float32)
    for idx, c in enumerate(mlp.classes_):
        w3[:, c] = w3_raw[:, idx]
        b3[c] = b3_raw[idx]

    export_weights(w1, b1, w2, b2, w3, b3)


if __name__ == "__main__":
    main()
