#!/usr/bin/env python3
"""
sweep_runner.py - standalone SNR sweep driver (no browser).

Generates continuous RTTY with per-segment AWGN and plays the whole buffer
to a chosen audio output device via `sounddevice`. Emits a sweep-log in the
exact format produced by the HTML simulator's textarea so `cer_analyze.py`
can consume it directly.

Typical run:
    python tools/sweep_runner.py \
        --device "LEN Q27h-10" \
        --from 20 --to -16 --step 2 --dwell 30 \
        --out-sweep datasets/logs/auto_sweep.txt

While running, prints per-point status to stderr:
    [BIN 3/19]  SNR=+16 dB  (elapsed 60.0s, remaining 540.0s)

Pair with `serial_logger.py --path-cycle 10` on COM27 to cycle PATH A/B/HYB
inside every 30s bin and analyze with `cer_analyze.py`.
"""
import argparse
import os
import sys
import time
from datetime import datetime, timezone

import numpy as np

try:
    import sounddevice as sd
except ImportError:
    print("ERROR: pip install sounddevice", file=sys.stderr)
    sys.exit(1)

# Reuse generator from rtty_gen.py
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rtty_gen import generate_rtty


def iso_now():
    return datetime.now(timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z')


def pick_device(name_substr, prefer_hostapi=('MME', 'Windows DirectSound', 'Windows WASAPI')):
    """Pick lowest-index matching output device, preferring given hostapi order."""
    devices = sd.query_devices()
    hostapis = [sd.query_hostapis(i)['name'] for i in range(len(sd.query_hostapis()))]
    matches = []
    for i, d in enumerate(devices):
        if d['max_output_channels'] <= 0:
            continue
        if name_substr.lower() not in d['name'].lower():
            continue
        matches.append((i, d, hostapis[d['hostapi']]))
    if not matches:
        print(f"ERROR: no output device matches '{name_substr}'", file=sys.stderr)
        print("Run with --list-devices to see options.", file=sys.stderr)
        sys.exit(2)
    # Sort by hostapi preference.
    def prio(m):
        host = m[2]
        return prefer_hostapi.index(host) if host in prefer_hostapi else 99
    matches.sort(key=prio)
    i, d, host = matches[0]
    print(f"[runner] output device [{i}] {d['name']} ({host}, "
          f"{int(d['default_samplerate'])} Hz, {d['max_output_channels']} ch)",
          file=sys.stderr)
    return i


def add_awgn(signal, snr_db, rng):
    """AWGN at given SNR (audio-band RMS)."""
    sig_rms = np.sqrt(np.mean(signal ** 2))
    if sig_rms <= 0 or snr_db is None:
        return signal
    noise_rms = sig_rms * 10 ** (-snr_db / 20.0)
    return signal + rng.normal(0.0, noise_rms, signal.shape)


def main():
    ap = argparse.ArgumentParser(description="Standalone SNR sweep driver.")
    ap.add_argument('--list-devices', action='store_true')
    ap.add_argument('--device', default='LEN Q27h-10',
                    help='Output device name substring (default: LEN Q27h-10)')
    ap.add_argument('--text', default='RYRYRY THE QUICK BROWN FOX JUMPS OVER 1234567890\r\n')
    ap.add_argument('--baud',  type=float, default=45.45)
    ap.add_argument('--shift', type=float, default=170.0)
    ap.add_argument('--stop',  type=float, default=1.5)
    ap.add_argument('--center', type=float, default=1500.0)
    ap.add_argument('--sr',    type=int, default=48000)
    ap.add_argument('--from',  dest='snr_from', type=float, default=20)
    ap.add_argument('--to',    dest='snr_to',   type=float, default=-16)
    ap.add_argument('--step',  type=float, default=2)
    ap.add_argument('--dwell', type=float, default=30.0, help='seconds per SNR point')
    ap.add_argument('--out-sweep', default=None, help='sweep-log file (simulator format)')
    ap.add_argument('--sig-level', type=float, default=0.10,
                    help='signal peak amplitude 0..1 (default 0.10 = -20 dBFS, leaves headroom for AWGN)')
    ap.add_argument('--seed',  type=int, default=None)
    args = ap.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        return

    dev_idx = pick_device(args.device)

    # Build SNR ladder (inclusive of endpoints, step direction from from→to).
    step = -abs(args.step) if args.snr_to < args.snr_from else abs(args.step)
    snrs = []
    s = args.snr_from
    while (step < 0 and s >= args.snr_to - 1e-9) or (step > 0 and s <= args.snr_to + 1e-9):
        snrs.append(round(s, 1))
        s += step
    N = len(snrs)
    total_dur = N * args.dwell
    print(f"[runner] {N} SNR points from {args.snr_from:+g} to {args.snr_to:+g} dB "
          f"step={step:+g} dwell={args.dwell}s total={total_dur:.0f}s "
          f"({total_dur/60:.1f} min)", file=sys.stderr)

    # Generate continuous RTTY for the whole sweep (phase-continuous).
    print(f"[runner] generating {total_dur:.0f}s of RTTY @ {args.baud} Bd, shift={args.shift} Hz, "
          f"center={args.center} Hz, sr={args.sr}", file=sys.stderr)
    clean, _, nsym = generate_rtty(args.text, args.baud, args.shift, args.stop,
                                    args.center, args.sr, total_dur)
    if len(clean) < int(total_dur * args.sr):
        pad = int(total_dur * args.sr) - len(clean)
        clean = np.concatenate([clean, np.zeros(pad)])
    # Pre-scale clean signal so AWGN at the lowest SNR fits within ±1 with ≲5% clipping.
    # generate_rtty emits peak=0.5 (≈-6 dBFS). We rescale to `sig_level` so that at the
    # worst SNR in the ladder, noise_rms = sig_rms·10^(-SNR/20) stays ≲0.47 (3σ→1.4; P(|x|>1)≈3%).
    clean_peak = float(np.max(np.abs(clean)))
    if clean_peak > 0:
        clean = clean * (args.sig_level / clean_peak)
    clean_rms = float(np.sqrt(np.mean(clean ** 2)))
    clean_peak_after = float(np.max(np.abs(clean)))
    print(f"[runner] generated {len(clean)} samples ({len(clean)/args.sr:.1f}s), "
          f"{nsym} Baudot symbols, signal peak={clean_peak_after:.3f} "
          f"({20*np.log10(clean_peak_after):+.1f} dBFS), rms={clean_rms:.3f}",
          file=sys.stderr)

    # Apply per-segment AWGN at each SNR point, with per-segment clip protection.
    # Per-segment (not global) rescale: high-SNR bins keep the full -20 dBFS signal
    # level so the decoder sees a realistic input, while only noisy low-SNR bins
    # get attenuated when noise peaks exceed headroom. SNR is preserved within
    # each segment because signal and noise scale by the same factor.
    rng = np.random.default_rng(args.seed)
    seg_len = int(args.dwell * args.sr)
    audio = np.zeros(len(clean), dtype=np.float64)
    rescaled_bins = []
    for i, snr in enumerate(snrs):
        a = i * seg_len
        b = min(a + seg_len, len(clean))
        seg = add_awgn(clean[a:b], snr, rng)
        seg_peak = float(np.max(np.abs(seg)))
        if seg_peak > 0.95:
            scale = 0.95 / seg_peak
            seg = seg * scale
            rescaled_bins.append((snr, seg_peak, scale))
        audio[a:b] = seg
    if rescaled_bins:
        worst = max(rescaled_bins, key=lambda r: r[1])
        print(f"[runner] per-segment rescale on {len(rescaled_bins)} of {len(snrs)} bins "
              f"(worst: SNR={worst[0]:+.0f} dB peak={worst[1]:.2f} x{worst[2]:.3f}) — "
              f"SNR preserved within each bin", file=sys.stderr)
    audio_f32 = audio.astype(np.float32)

    # Open sweep-log.
    sweep_f = None
    if args.out_sweep:
        os.makedirs(os.path.dirname(os.path.abspath(args.out_sweep)), exist_ok=True)
        sweep_f = open(args.out_sweep, 'w', encoding='utf-8', buffering=1)

    def emit(line):
        print(line, file=sys.stderr)
        if sweep_f:
            sweep_f.write(line + '\n')

    t0_iso = iso_now()
    header = f"=== SWEEP START {t0_iso} ==="
    param  = f"from={int(args.snr_from)} to={int(args.snr_to)} step={int(abs(args.step))} dwell={int(args.dwell)}s points={N}"
    emit(header)
    emit(param)

    # Start playback non-blocking, then emit per-bin timestamps as wall clock advances.
    print(f"[runner] starting playback on device [{dev_idx}]...", file=sys.stderr)
    try:
        sd.play(audio_f32, samplerate=args.sr, device=dev_idx, blocking=False)
    except Exception as e:
        print(f"[runner] sd.play failed: {e}", file=sys.stderr)
        sys.exit(3)

    t_start = time.time()
    for i, snr in enumerate(snrs):
        target_t = t_start + i * args.dwell
        now = time.time()
        if target_t > now:
            time.sleep(target_t - now)
        sign = '+' if snr >= 0 else '-'
        line = f"{iso_now()}  SNR={sign}{abs(int(snr))} dB  [{i+1}/{N}]"
        emit(line)
        elapsed = (i + 1) * args.dwell
        remaining = total_dur - elapsed
        print(f"    [progress] bin {i+1}/{N}  SNR={sign}{abs(int(snr)):>2d} dB  "
              f"elapsed={elapsed:.0f}s remaining={remaining:.0f}s", file=sys.stderr)

    # Wait for tail of last segment.
    try:
        sd.wait()
    except KeyboardInterrupt:
        print("[runner] interrupted — stopping audio", file=sys.stderr)
        sd.stop()
    emit(f"=== SWEEP DONE {iso_now()} ===")
    if sweep_f:
        sweep_f.close()
        print(f"[runner] sweep log written: {args.out_sweep}", file=sys.stderr)


if __name__ == '__main__':
    main()
