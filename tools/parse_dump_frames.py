#!/usr/bin/env python3
"""parse_dump_frames.py - extract FR records from bench_replay logs.

Reads logs produced by DUMP FRAMES ON, finds FR <s0..s6> <sig> <hard_char>
lines anywhere on the timestamped line, writes a numpy .npz with:
  X: (N, 7) bipolar soft-bits normalized by sig_level
  y: (N,)    hard_char labels (0-31)
  sig: (N,)  raw sig_level
  data_min: (N,) min |data bit| / sig (gate-like uncertainty score)

Usage:
    parse_dump_frames.py LOG [LOG ...] --out training_real.npz
"""
import argparse
import re
import sys
import numpy as np
from pathlib import Path

# Pattern: FR <8 floats> <int>
FR_RE = re.compile(
    r"FR\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)\s+"
    r"(-?\d+\.\d+)\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)\s+(-?\d+\.\d+)\s+(\d+)"
)


def parse_log(path: Path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            for m in FR_RE.finditer(line):
                vals = [float(m.group(i)) for i in range(1, 9)] + [int(m.group(9))]
                rows.append(vals)
    if not rows:
        return None
    arr = np.array(rows, dtype=np.float32)
    return arr  # (N, 10): 7 soft-bits, sig, ?, hard_char (last col is char as float)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    all_rows = []
    for p in args.logs:
        arr = parse_log(Path(p))
        if arr is None:
            print(f"[warn] no FR records in {p}", file=sys.stderr)
            continue
        print(f"[+] {p}: {len(arr)} FR records")
        all_rows.append(arr)
    arr = np.concatenate(all_rows, axis=0)

    # arr columns: 0..6 = soft bits, 7 = sig_level, 8 = hard_char (parsed as float)
    soft = arr[:, 0:7]
    sig = arr[:, 7]
    y = arr[:, 8].astype(np.int32)

    # Normalize soft bits by sig (avoid div by tiny sig)
    sig_safe = np.where(sig > 1e-3, sig, 1e-3)
    X = (soft / sig_safe[:, None]).astype(np.float32)

    # data_min = min |data bit| / sig (bits 1..5)
    data_min = np.min(np.abs(X[:, 1:6]), axis=1).astype(np.float32)

    # Filter sane labels (0..31)
    valid = (y >= 0) & (y < 32) & (sig > 1.0)
    X, y, sig, data_min = X[valid], y[valid], sig[valid], data_min[valid]

    print(f"\n[*] Total: {len(X)} frames")
    print(f"[*] Label distribution:")
    bins = np.bincount(y, minlength=32)
    for i, n in enumerate(bins):
        if n > 0:
            print(f"    char={i:2d}  count={n:6d}")
    print(f"\n[*] sig:      min={sig.min():.2f}  median={np.median(sig):.2f}  max={sig.max():.2f}")
    print(f"[*] data_min: min={data_min.min():.3f}  median={np.median(data_min):.3f}  max={data_min.max():.3f}")
    print(f"[*] data_min < 0.20: {(data_min < 0.20).sum()} frames "
          f"(== inference-gate-fire frames)")
    print(f"[*] 0.20 <= data_min < 0.40: {((data_min >= 0.20) & (data_min < 0.40)).sum()} frames")
    print(f"[*] data_min >= 0.40: {(data_min >= 0.40).sum()} frames")

    np.savez_compressed(args.out, X=X, y=y, sig=sig, data_min=data_min)
    print(f"\n[+] Saved {args.out}  ({Path(args.out).stat().st_size/1024:.1f} KB)")


if __name__ == "__main__":
    main()
