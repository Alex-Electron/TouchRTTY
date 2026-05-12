#!/usr/bin/env python3
"""train_nn_torch.py — PyTorch version of train_nn.py for advanced training tricks.

Replicates v4 architecture (7 -> 128 -> 64 -> 32) and synthetic data recipe
but adds knobs that sklearn MLPClassifier doesn't support:

  - label smoothing (CrossEntropyLoss(label_smoothing=...))
  - sample_weight (per-frame loss weighting; boost uncertain frames)
  - dropout
  - explicit L2 weight decay
  - longer training with warmup + cosine schedule

Exports identical C-header format so existing flash/bench pipeline reuses.

Usage:
    python tools/train_nn_torch.py [--label-smoothing 0.05]
                                   [--weight-uncertain 3.0]
                                   [--dropout 0.1]
                                   [--weight-decay 3e-4]
                                   [--epochs 80]
                                   [--real-npz path.npz]
                                   [--real-replicate N]
                                   [--n-synth 15000]
                                   [--out src/dsp/nn_weights.h]
"""
from __future__ import annotations
import argparse
import math
import os
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, TensorDataset

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

# Reuse the synthetic generator + C-export helpers from sklearn version.
from train_nn import generate_synthetic, format_array_c  # noqa: E402

HIDDEN1 = int(os.environ.get("TRAIN_NN_H1", 128))
HIDDEN2 = int(os.environ.get("TRAIN_NN_H2", 64))
N_CLASSES = 32
N_INPUT = 7


class MLP(nn.Module):
    def __init__(self, h1=HIDDEN1, h2=HIDDEN2, dropout=0.0):
        super().__init__()
        self.fc1 = nn.Linear(N_INPUT, h1)
        self.fc2 = nn.Linear(h1, h2)
        self.fc3 = nn.Linear(h2, N_CLASSES)
        self.dropout = nn.Dropout(dropout) if dropout > 0 else nn.Identity()

    def forward(self, x):
        x = torch.relu(self.fc1(x))
        x = self.dropout(x)
        x = torch.relu(self.fc2(x))
        x = self.dropout(x)
        return self.fc3(x)


def export_header(model: MLP, out_path: Path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    w1 = model.fc1.weight.detach().cpu().numpy().T  # (7, H1) to match sklearn layout
    b1 = model.fc1.bias.detach().cpu().numpy()
    w2 = model.fc2.weight.detach().cpu().numpy().T  # (H1, H2)
    b2 = model.fc2.bias.detach().cpu().numpy()
    w3 = model.fc3.weight.detach().cpu().numpy().T  # (H2, 32)
    b3 = model.fc3.bias.detach().cpu().numpy()

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("#ifndef NN_WEIGHTS_H\n#define NN_WEIGHTS_H\n\n")
        f.write("// Auto-generated TinyML weights (PyTorch) — DO NOT EDIT MANUALLY\n")
        f.write(f"// Architecture: Input({N_INPUT}) -> Dense({HIDDEN1}, ReLU)"
                f" -> Dense({HIDDEN2}, ReLU) -> Dense({N_CLASSES}, Linear)\n\n")
        f.write(f"#define NN_INPUT  {N_INPUT}\n")
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
    total = (N_INPUT*HIDDEN1 + HIDDEN1 + HIDDEN1*HIDDEN2 + HIDDEN2 +
             HIDDEN2*N_CLASSES + N_CLASSES)
    print(f"[+] Weights saved to {out_path}")
    print(f"[+] Total parameters: {total} floats = {total*4/1024:.1f} KB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label-smoothing", type=float, default=0.0)
    ap.add_argument("--weight-uncertain", type=float, default=1.0,
                    help="loss-weight multiplier for frames with data_min < 0.30 (1=disabled)")
    ap.add_argument("--dropout", type=float, default=0.0)
    ap.add_argument("--weight-decay", type=float, default=3e-4)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--batch-size", type=int, default=1024)
    ap.add_argument("--epochs", type=int, default=80)
    ap.add_argument("--early-patience", type=int, default=12)
    ap.add_argument("--n-synth", type=int, default=15000)
    ap.add_argument("--real-npz", action="append", default=[])
    ap.add_argument("--real-replicate", type=int, default=1)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out", default="src/dsp/nn_weights.h")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)

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

    # Per-sample weight: uncertain frames get higher weight if weight_uncertain > 1.
    data_min = np.min(np.abs(X[:, 1:6]), axis=1)
    weights = np.ones(len(X), dtype=np.float32)
    if args.weight_uncertain > 1.0:
        weights[data_min < 0.30] = args.weight_uncertain
        boosted = int((data_min < 0.30).sum())
        print(f"[*] Boosted {boosted}/{len(X)} uncertain frames by x{args.weight_uncertain}")

    # 90/10 train/val split (shuffled)
    rng = np.random.default_rng(args.seed)
    idx = rng.permutation(len(X))
    split = int(0.9 * len(idx))
    tr_idx, va_idx = idx[:split], idx[split:]

    Xt = torch.from_numpy(X[tr_idx])
    yt = torch.from_numpy(y[tr_idx]).long()
    wt = torch.from_numpy(weights[tr_idx])
    Xv = torch.from_numpy(X[va_idx])
    yv = torch.from_numpy(y[va_idx]).long()

    train_ds = TensorDataset(Xt, yt, wt)
    train_dl = DataLoader(train_ds, batch_size=args.batch_size, shuffle=True,
                          num_workers=0, pin_memory=False)
    print(f"[*] Train={len(tr_idx)}  Val={len(va_idx)}  arch=7->{HIDDEN1}->{HIDDEN2}->32")

    model = MLP(dropout=args.dropout)
    optim = torch.optim.Adam(model.parameters(), lr=args.lr,
                             weight_decay=args.weight_decay)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(optim, T_max=args.epochs)
    loss_fn = nn.CrossEntropyLoss(label_smoothing=args.label_smoothing,
                                  reduction="none")

    best_val = 0.0
    best_state = None
    patience = 0
    print(f"[*] Training (label_smoothing={args.label_smoothing}, "
          f"weight_uncertain={args.weight_uncertain}, dropout={args.dropout}, "
          f"wd={args.weight_decay}, lr={args.lr}, epochs={args.epochs})...")

    for ep in range(args.epochs):
        model.train()
        for xb, yb, wb in train_dl:
            optim.zero_grad()
            out = model(xb)
            per_sample = loss_fn(out, yb)
            loss = (per_sample * wb).sum() / wb.sum()
            loss.backward()
            optim.step()
        sched.step()

        model.eval()
        with torch.no_grad():
            pred = model(Xv).argmax(1)
            val_acc = (pred == yv).float().mean().item()

        if val_acc > best_val + 1e-4:
            best_val = val_acc
            best_state = {k: v.clone() for k, v in model.state_dict().items()}
            patience = 0
            print(f"  ep {ep+1:3d}/{args.epochs}  val_acc={val_acc*100:.2f}%  (best, "
                  f"lr={sched.get_last_lr()[0]:.4f})")
        else:
            patience += 1
            if ep % 5 == 0:
                print(f"  ep {ep+1:3d}/{args.epochs}  val_acc={val_acc*100:.2f}%  "
                      f"(p={patience})")
            if patience >= args.early_patience:
                print(f"  early-stop at ep {ep+1}")
                break

    model.load_state_dict(best_state)
    print(f"[+] Best validation accuracy: {best_val*100:.2f}%")

    export_header(model, Path(args.out))


if __name__ == "__main__":
    main()
