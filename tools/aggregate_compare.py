#!/usr/bin/env python3
"""aggregate_compare.py - aggregate compare.txt from N seed runs into mean ± std.

Usage: aggregate_compare.py DIR [DIR ...]
"""
import sys
import re
from pathlib import Path
from collections import defaultdict
from statistics import mean, stdev


def parse_compare(p):
    """parse SNR/NN_OFF/NN_ON triples from compare.txt"""
    rows = {}
    with open(p) as f:
        for line in f:
            m = re.match(r"\s*(-?\d+)\s+([\d.]+)%\s+([\d.]+)%", line)
            if m:
                snr = int(m.group(1))
                rows[snr] = (float(m.group(2)), float(m.group(3)))
    return rows


def main():
    dirs = sys.argv[1:]
    all_off = defaultdict(list)
    all_on = defaultdict(list)
    for d in dirs:
        p = Path(d) / "compare.txt"
        if not p.exists():
            print(f"[warn] missing {p}", file=sys.stderr)
            continue
        for snr, (off, on) in parse_compare(p).items():
            all_off[snr].append(off)
            all_on[snr].append(on)

    if not all_off:
        print("no data"); return

    n = len(dirs)
    print(f"\n  SNR | NN OFF mean (sd)    | NN ON  mean (sd)    | delta")
    print(f"  ----+---------------------+---------------------+--------")
    for snr in sorted(all_off, reverse=True):
        off_m = mean(all_off[snr]); off_s = stdev(all_off[snr]) if len(all_off[snr])>1 else 0
        on_m  = mean(all_on[snr]);  on_s  = stdev(all_on[snr])  if len(all_on[snr]) >1 else 0
        delta = on_m - off_m
        print(f"  {snr:3d} | {off_m:6.2f}% (±{off_s:4.2f})    "
              f"| {on_m:6.2f}% (±{on_s:4.2f})    | {delta:+6.2f}")
    print(f"\n  ({n} seeds)")


if __name__ == "__main__":
    main()
