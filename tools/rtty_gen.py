#!/usr/bin/env python3
"""
rtty_gen.py - RTTY WAV generator with known text and SNR control.

Usage:
    python rtty_gen.py --text "CQCQCQ DE TEST RYRYRY" \
        --baud 45.45 --shift 170 --stop 1.5 \
        --center 1500 --snr 0 --duration 30 \
        --sr 48000 --out datasets/generated/test.wav

Output:
    WAV 16-bit mono, amplitude ~0.5 to leave headroom for added noise.
    Ground-truth text is printed to stdout (for comparison with decoded serial output).

Conventions match the HTML simulator (`tools/rtty_simulator.html`):
    - ITA2 Baudot, LSB first, start=Space, stop=Mark.
    - FIGS/LTRS shift auto-inserted on mode switch.
    - AWGN in audio-band (not bit-bandwidth) - matches HTML.
"""
import argparse
import sys
import numpy as np
from scipy.io import wavfile

LETTERS = "\x00E\nA SIU\rDRJNFCKTZLWHYPQOBG\x00MXV\x00"
FIGURES = "\x003\n- '87\r$4',!:(5\")2#6019?&\x00./=\x00"
SHIFT_LETTERS = 0x1F
SHIFT_FIGURES = 0x1B


def encode_baudot(text):
    """Return list of 5-bit Baudot codes with FIGS/LTRS shifts."""
    out = []
    shift = 'LETTERS'
    for ch in text.upper():
        if ch in LETTERS and (ch in (' ', '\r', '\n') or shift == 'LETTERS' or ch not in FIGURES):
            if ch in LETTERS:
                if shift != 'LETTERS' and ch not in (' ', '\r', '\n'):
                    out.append(SHIFT_LETTERS)
                    shift = 'LETTERS'
                out.append(LETTERS.index(ch))
                continue
        if ch in FIGURES:
            if shift != 'FIGURES' and ch not in (' ', '\r', '\n'):
                out.append(SHIFT_FIGURES)
                shift = 'FIGURES'
            out.append(FIGURES.index(ch))
    return out


def generate_rtty(text, baud, shift_hz, stop_bits, center, sr, duration):
    bit_time = 1.0 / baud
    f_mark  = center - shift_hz / 2.0
    f_space = center + shift_hz / 2.0
    codes = encode_baudot(text)

    # Build frequency schedule: list of (freq, samples).
    segments = []
    # Idle preamble - 0.5s of mark tone.
    segments.append((f_mark, int(0.5 * sr)))
    n_bits = int(bit_time * sr)
    n_stop = int(bit_time * stop_bits * sr)

    total_symbols = 0
    pos = int(0.5 * sr)
    target = int(duration * sr)
    char_idx = 0
    while pos < target:
        if not codes:
            break
        code = codes[char_idx % len(codes)]
        char_idx += 1
        total_symbols += 1
        # Start bit = Space
        segments.append((f_space, n_bits))
        pos += n_bits
        # Data bits, LSB first
        for i in range(5):
            bit = (code >> i) & 1
            segments.append((f_mark if bit else f_space, n_bits))
            pos += n_bits
        # Stop bit(s) = Mark
        segments.append((f_mark, n_stop))
        pos += n_stop

    # Continuous-phase sine synthesis.
    n_total = sum(s[1] for s in segments)
    n_total = min(n_total, int(duration * sr))
    out = np.zeros(n_total, dtype=np.float64)
    phase = 0.0
    idx = 0
    for freq, n in segments:
        if idx >= n_total:
            break
        n = min(n, n_total - idx)
        t = np.arange(n)
        dphi = 2.0 * np.pi * freq / sr
        out[idx:idx+n] = 0.5 * np.sin(phase + dphi * t)
        phase += dphi * n
        idx += n
    return out, char_idx, total_symbols


def add_awgn(signal, snr_db):
    """Add AWGN in audio-band. signal_rms / noise_rms = 10^(SNR/20)."""
    sig_rms = np.sqrt(np.mean(signal ** 2))
    if sig_rms <= 0:
        return signal
    noise_rms = sig_rms * 10 ** (-snr_db / 20.0)
    noise = np.random.normal(0.0, noise_rms, signal.shape)
    return signal + noise


def main():
    ap = argparse.ArgumentParser(description="RTTY WAV generator with SNR control.")
    ap.add_argument('--text',    default="RYRYRY THE QUICK BROWN FOX 1234567890 ")
    ap.add_argument('--baud',    type=float, default=45.45)
    ap.add_argument('--shift',   type=float, default=170.0, help='Hz')
    ap.add_argument('--stop',    type=float, default=1.5)
    ap.add_argument('--center',  type=float, default=1500.0, help='Hz')
    ap.add_argument('--snr',     type=float, default=None, help='dB (omit for no noise)')
    ap.add_argument('--sr',      type=int,   default=48000, help='sample rate Hz')
    ap.add_argument('--duration', type=float, default=30.0, help='seconds')
    ap.add_argument('--out',     required=True)
    args = ap.parse_args()

    sig, chars_transmitted, symbols = generate_rtty(
        args.text, args.baud, args.shift, args.stop, args.center, args.sr, args.duration)
    print(f"Generated {len(sig)/args.sr:.2f}s audio, "
          f"{symbols} Baudot symbols ({chars_transmitted} chars from loop).",
          file=sys.stderr)

    if args.snr is not None:
        sig = add_awgn(sig, args.snr)
        print(f"AWGN applied, target SNR = {args.snr:+.1f} dB.", file=sys.stderr)

    # Clip-protect before int16 scaling.
    peak = np.max(np.abs(sig))
    if peak > 0.98:
        sig = sig * (0.98 / peak)
        print(f"Clip-protect: scaled by {0.98/peak:.3f}", file=sys.stderr)
    sig_i16 = (sig * 32767).astype(np.int16)
    wavfile.write(args.out, args.sr, sig_i16)
    print(f"Written: {args.out} ({args.sr} Hz, {len(sig_i16)} samples)", file=sys.stderr)

    # Ground-truth: the chars that were actually transmitted within the duration window.
    print("--- GROUND TRUTH ---")
    repeats = chars_transmitted // len(args.text) + 1
    gt = (args.text * max(1, repeats))[:chars_transmitted]
    print(gt)


if __name__ == '__main__':
    main()
