#!/usr/bin/env python3
"""
cer_analyze.py - correlate HTML sweep log + serial log and compute CER(SNR).

Inputs:
    --sweep  <file>   HTML simulator sweep log (copy textarea to .txt)
    --serial <file>   serial_logger.py output
    --gt     <string or @file>  ground-truth text (the cycle that was TX'd)
    --out    <file>   CSV table (optional)
    --plot   <file>   PNG plot (optional, needs matplotlib)

Workflow:
    1. rtty_simulator.html: SWEEP produces log in the textarea.
       Save as datasets/logs/sweep_<date>.txt.
    2. serial_logger.py captures decoded output with timestamps.
    3. cer_analyze.py correlates the two by ISO timestamps and
       computes CER per SNR point via Levenshtein over best cyclic
       alignment of ground-truth.

Sweep-log line format:
    2026-04-13T14:30:00.100Z  SNR=+20 dB  [1/18]

Serial-log line format:
    2026-04-13T14:30:01.234\tDECODED_TEXT

Output: table to stdout + optional CSV + optional PNG.
"""
import argparse
import csv
import re
import sys
from datetime import datetime, timezone


SWEEP_POINT_RE = re.compile(
    r'^(?P<ts>\S+Z)\s+SNR=(?P<sign>[+\-])?(?P<val>\d+)\s*dB\s+\[(?P<idx>\d+)/(?P<total>\d+)\]')
PATH_CMD_RE = re.compile(r'\[CMD\]\s+PATH=(?P<name>\S+)')


def parse_iso(ts):
    # Accept trailing Z or offset. Python 3.11+ handles Z directly in fromisoformat.
    if ts.endswith('Z'):
        ts = ts[:-1] + '+00:00'
    return datetime.fromisoformat(ts)


def parse_sweep(path):
    points = []
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            m = SWEEP_POINT_RE.match(line.strip())
            if not m:
                continue
            sign = m.group('sign') or '+'
            snr = int(m.group('val')) * (-1 if sign == '-' else 1)
            points.append({
                'ts': parse_iso(m.group('ts')),
                'snr': snr,
                'idx': int(m.group('idx')),
                'total': int(m.group('total')),
            })
    return points


def parse_serial(path):
    """Return (content_records, path_switches).
    content_records: [(datetime, text), ...] — decoded text lines.
    path_switches:   [(datetime, 'A'|'B'|'HYB'|...), ...] — PATH=X commands from
                     serial_logger --path-cycle, used for per-PATH binning.
    """
    records = []
    switches = []
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if not line or line.startswith('==='):
                continue
            parts = line.split('\t', 1)
            if len(parts) != 2:
                continue
            ts_str, text = parts
            try:
                ts = parse_iso(ts_str)
            except ValueError:
                continue
            m = PATH_CMD_RE.search(text)
            if m:
                switches.append((ts, m.group('name')))
            else:
                records.append((ts, text))
    return records, switches


def clean_decoded(text):
    """Strip firmware diagnostic/command noise, keep ASCII printable decoded text only.

    Firmware emits decoded chars interleaved with diag lines like:
        `ICK[D] SNR=81.2 SIG=-20.5 ERR=0% SQ=OPEN ...`
        `>> UNKNOWN: PATH A`        `>> AUTO-INV: Flipped to INV`
        `[HYBRID DIVERGENCE: ...]`  `[FIGS] [LTRS] [ERR]`
    We strip anything inside [...] (incl. [D] diag blocks running to newline)
    and anything from `>>` to end-of-line.
    """
    # 1) Drop bracketed diagnostics. [D] starts a block that runs until the end
    #    of the line (no closing bracket). Kill everything from `[D]` to \n first,
    #    then strip any remaining [XXX] short tags.
    text = re.sub(r'\[D\][^\n]*', '', text)
    text = re.sub(r'\[HYBRID[^\]\n]*\]?[^\n]*', '', text)
    text = re.sub(r'\[[A-Z]+[^\]]*\]', '', text)
    # 2) Drop >> command-response lines.
    text = re.sub(r'>>[^\n]*', '', text)
    # 3) Drop === log markers.
    text = re.sub(r'===[^\n]*', '', text)
    out = []
    for ch in text:
        if ch == '\r':
            continue
        if ch == '\n':
            out.append(' ')
            continue
        if 0x20 <= ord(ch) <= 0x7E:
            out.append(ch.upper())
    return ''.join(out)


def levenshtein(a, b):
    if len(a) < len(b):
        a, b = b, a
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        curr = [i]
        for j, cb in enumerate(b, 1):
            ins = curr[j-1] + 1
            dele = prev[j] + 1
            sub = prev[j-1] + (ca != cb)
            curr.append(min(ins, dele, sub))
        prev = curr
    return prev[-1]


def best_cer(decoded, gt_cycle):
    """Ground-truth repeats cyclically. Build a gt window of len(decoded) at each offset,
    return min edit distance normalized by gt length.
    Optimization: first-pass character-match scoring to pick top-3 offsets,
    then full Levenshtein only on those — O(49·N + 3·N²) vs O(49·N²)."""
    if not decoded:
        return 1.0
    L = len(decoded)
    G = len(gt_cycle)
    if not gt_cycle:
        return 1.0
    # Pre-build expanded gt for fast window slicing.
    reps = (L // G) + 2
    gt_ext = gt_cycle * reps
    # Score all offsets by matching-char count (cheap O(L) per offset).
    scores = []
    for off in range(G):
        win = gt_ext[off:off + L]
        matches = sum(1 for a, b in zip(decoded, win) if a == b)
        scores.append((matches, off))
    scores.sort(reverse=True)
    top = scores[:3]
    best = None
    for _, off in top:
        win = gt_ext[off:off + L]
        d = levenshtein(decoded, win)
        if best is None or d < best:
            best = d
    return best / L


def load_gt(spec):
    if spec.startswith('@'):
        with open(spec[1:], 'r', encoding='utf-8') as f:
            s = f.read()
    else:
        s = spec
    return clean_decoded(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sweep',  required=True, help='HTML sweep log (textarea export)')
    ap.add_argument('--serial', required=True, help='serial_logger.py output')
    ap.add_argument('--gt',     required=True, help='ground-truth string or @file')
    ap.add_argument('--out',    default=None, help='CSV output (optional)')
    ap.add_argument('--plot',   default=None, help='PNG plot (optional, needs matplotlib)')
    ap.add_argument('--trim',   type=float, default=3.0,
                    help='seconds to skip at start of each window (decoder settle)')
    ap.add_argument('--lag',    type=float, default=2.0,
                    help='serial-log batching lag: chars flushed at [CMD] belong to prev path/bin')
    ap.add_argument('--markers', action='store_true',
                    help='Use inline =NN= sync markers (B241+) instead of timestamps. '
                         'Robust to serial-log batching.')
    args = ap.parse_args()

    points = parse_sweep(args.sweep)
    if len(points) < 2:
        print(f"ERROR: sweep has {len(points)} points, need >=2", file=sys.stderr)
        sys.exit(1)
    serial_recs, switches = parse_serial(args.serial)
    gt = load_gt(args.gt)
    print(f"Parsed {len(points)} sweep points, {len(serial_recs)} serial records, "
          f"{len(switches)} PATH-switches.", file=sys.stderr)
    # Path intervals: [ts, next_ts) -> path-name. Empty = no --path-cycle in logger.
    path_intervals = []
    for i, (ts, name) in enumerate(switches):
        t_s = ts.timestamp()
        t_e = switches[i+1][0].timestamp() if i+1 < len(switches) else t_s + 3600
        path_intervals.append((t_s, t_e, name))
    per_path = bool(path_intervals)
    path_names = sorted(set(n for _, _, n in path_intervals)) if per_path else ['']
    print(f"Ground truth ({len(gt)} chars cyclic): {gt[:80]}{'...' if len(gt) > 80 else ''}",
          file=sys.stderr)

    results = []
    if args.markers:
        # Marker-based binning — robust to serial-log batching.
        # Concatenate all serial text, locate =NN= markers, slice between them.
        if per_path:
            print("WARNING: --markers ignores --path-cycle PATH-binning (use timestamp mode).",
                  file=sys.stderr)
        full = clean_decoded(' '.join(t for _, t in serial_recs))
        marker_re = re.compile(r'=\s*(\d{2})\s*=')
        hits = [(m.start(), int(m.group(1))) for m in marker_re.finditer(full)]
        print(f"Found {len(hits)} markers in decoded stream: "
              f"{[n for _, n in hits[:20]]}{'...' if len(hits) > 20 else ''}",
              file=sys.stderr)
        # Map marker N (1-based) → segment [end_of_marker_N, start_of_marker_N+1).
        for i, p in enumerate(points):
            n = p['idx']
            cur = next((h for h in hits if h[1] == n), None)
            nxt = next((h for h in hits if h[1] == n + 1), None)
            if cur is None:
                segment = ''
            else:
                seg_start = cur[0] + len(f'={n:02d}=')
                seg_end = nxt[0] if nxt else len(full)
                segment = full[seg_start:seg_end]
            # Strip any leftover marker-like substrings inside segment.
            segment = marker_re.sub('', segment).strip()
            cer = best_cer(segment, gt) if segment else 1.0
            results.append({
                'idx': n, 'snr': p['snr'], 'path': '',
                'decoded_len': len(segment),
                'cer': cer,
                'decoded_preview': segment[:60],
            })
    else:
        # Legacy timestamp-based binning. If --path-cycle was used by logger,
        # split each SNR-window further per PATH.
        def pick_path(tsec):
            for a, b, n in path_intervals:
                if a <= tsec < b:
                    return n
            return None
        # Shift record timestamps back by --lag to compensate for serial-log batching:
        # firmware flushes buffered chars only when a newline arrives (typically at the
        # next [CMD] PATH=X echo). So chars decoded during path/bin N appear in the log
        # under a timestamp belonging to path/bin N+1. Subtracting lag reassigns them.
        lag = args.lag
        for i, p in enumerate(points):
            t_start = p['ts'].timestamp() + args.trim
            t_end   = points[i+1]['ts'].timestamp() if i + 1 < len(points) \
                      else t_start + 300
            for pname in path_names:
                chars = []
                for ts, text in serial_recs:
                    tsec = ts.timestamp() - lag
                    if not (t_start <= tsec < t_end):
                        continue
                    if per_path and pick_path(tsec) != pname:
                        continue
                    chars.append(text)
                decoded = ''.join(clean_decoded(c) for c in chars)
                cer = best_cer(decoded, gt)
                results.append({
                    'idx': p['idx'], 'snr': p['snr'], 'path': pname,
                    'decoded_len': len(decoded),
                    'cer': cer,
                    'decoded_preview': decoded[:60],
                })

    # Print table.
    print()
    if per_path:
        print(f"{'idx':>3}  {'SNR':>5}  {'PATH':<4}  {'chars':>6}  {'CER':>7}  {'preview':<60}")
        print('-' * 96)
        for r in results:
            print(f"{r['idx']:>3}  {r['snr']:+4d}  {r['path']:<4}  {r['decoded_len']:>6}  "
                  f"{r['cer']:>6.2%}  {r['decoded_preview']:<60}")
        for pname in path_names:
            above = [r for r in results
                     if r['path'] == pname and r['cer'] >= 0.05 and r['decoded_len'] > 0]
            if above:
                threshold = max(r['snr'] for r in above)
                print(f"\nPATH {pname}: threshold (CER >= 5%) ~ {threshold:+d} dB")
            else:
                print(f"\nPATH {pname}: all non-empty points below 5% CER.")
    else:
        print(f"{'idx':>3}  {'SNR':>5}  {'chars':>6}  {'CER':>7}  {'preview':<60}")
        print('-' * 90)
        for r in results:
            print(f"{r['idx']:>3}  {r['snr']:+4d}  {r['decoded_len']:>6}  "
                  f"{r['cer']:>6.2%}  {r['decoded_preview']:<60}")
        above = [r for r in results if r['cer'] >= 0.05 and r['decoded_len'] > 0]
        if above:
            threshold = max(r['snr'] for r in above)
            print(f"\nDecoder threshold estimate (CER >= 5%): SNR ~ {threshold:+d} dB")
        else:
            print("\nAll points below 5% CER.")

    if args.out:
        with open(args.out, 'w', newline='', encoding='utf-8') as f:
            fields = ['idx', 'snr', 'path', 'decoded_len', 'cer', 'decoded_preview'] \
                     if per_path else ['idx', 'snr', 'decoded_len', 'cer', 'decoded_preview']
            w = csv.DictWriter(f, fieldnames=fields)
            w.writeheader()
            for r in results:
                w.writerow({k: r.get(k, '') for k in fields})
        print(f"CSV written: {args.out}", file=sys.stderr)

    if args.plot:
        try:
            import matplotlib
            matplotlib.use('Agg')
            import matplotlib.pyplot as plt
            snrs = [r['snr'] for r in results]
            cers = [r['cer'] * 100 for r in results]
            fig, ax = plt.subplots(figsize=(8, 5))
            ax.plot(snrs, cers, 'o-', linewidth=2, markersize=8)
            ax.axhline(5.0, color='red', linestyle='--', alpha=0.5, label='5% threshold')
            ax.set_xlabel('SNR (dB, audio-band)')
            ax.set_ylabel('CER (%)')
            ax.set_title('Decoder CER vs SNR')
            ax.grid(True, alpha=0.3)
            ax.legend()
            fig.tight_layout()
            fig.savefig(args.plot, dpi=100)
            print(f"Plot written: {args.plot}", file=sys.stderr)
        except ImportError:
            print("WARNING: matplotlib not installed, skipping plot.", file=sys.stderr)


if __name__ == '__main__':
    main()
