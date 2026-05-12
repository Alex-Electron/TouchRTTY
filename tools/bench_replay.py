#!/usr/bin/env python3
"""bench_replay.py - play WAV(s) through audio device, log TouchRTTY serial decode.

Usage:
    bench_replay.py --wavs file1.wav file2.wav ... --outdir datasets/logs/real_air/<tag>
    bench_replay.py --wav-dir tools/recs --glob '*.WAV' --outdir datasets/logs/...

Writes one log per WAV (lines: ISO8601\tdecoded-text), plus summary.md.
"""
import argparse
import datetime as dt
import glob
import os
import queue
import re
import sys
import threading
import time
import wave
from pathlib import Path

import numpy as np
import sounddevice as sd
import serial
from scipy.signal import find_peaks


ERR_PAT = re.compile(r"\[ERR\]")
CTRL_PAT = re.compile(r"\[(LTRS|FIGS)\]")


def find_rtty_center(path, target_shift=450, shift_tol=30):
    """Scan WAV in 2-sec windows, return center of best 2-tone pair separated by ~target_shift Hz."""
    with wave.open(path, "rb") as w:
        ch, sr, n = w.getnchannels(), w.getframerate(), w.getnframes()
        a = np.frombuffer(w.readframes(n), dtype=np.int16).reshape(-1, ch)
    if ch > 1:
        a = a.mean(axis=1).astype(np.int16)
    a = a.astype(np.float32)
    best = None
    N = 1 << 14
    for start in range(0, max(1, len(a) - 2 * sr), 2 * sr):
        seg = a[start:start + 2 * sr]
        if len(seg) < sr:
            continue
        buf = np.zeros(N, dtype=np.float32)
        chunk = seg[:N].reshape(-1) if seg.ndim > 1 else seg[:N]
        buf[:min(len(chunk), N)] = chunk[:N]
        spec = np.abs(np.fft.rfft(buf * np.hanning(N)))
        freqs = np.fft.rfftfreq(N, 1 / sr)
        mask = (freqs > 400) & (freqs < 3000)
        s = np.where(mask, spec, 0.0)
        if s.max() == 0:
            continue
        peaks, _ = find_peaks(s, distance=20, height=s.max() * 0.3)
        if len(peaks) < 2:
            continue
        best_pair = None
        best_err = 999
        for i in peaks:
            for j in peaks:
                if j <= i:
                    continue
                df = freqs[j] - freqs[i]
                if abs(df - target_shift) <= shift_tol and abs(df - target_shift) < best_err:
                    best_err = abs(df - target_shift)
                    best_pair = (i, j)
        if best_pair and (best is None or s[best_pair[0]] + s[best_pair[1]] > best[3]):
            best = (freqs[best_pair[0]], freqs[best_pair[1]],
                    (freqs[best_pair[0]] + freqs[best_pair[1]]) / 2,
                    s[best_pair[0]] + s[best_pair[1]])
    if best is None:
        return None
    return {"mark": float(best[0]), "space": float(best[1]), "center": float(best[2])}


def send_cmds(port, baud, cmds):
    with serial.Serial(port, baud, timeout=1) as s:
        time.sleep(0.3)
        s.reset_input_buffer()
        for c in cmds:
            s.write((c + "\r\n").encode())
            time.sleep(0.15)
        time.sleep(0.2)


def iso_now():
    return dt.datetime.now(dt.timezone.utc).astimezone().isoformat(timespec="milliseconds")


def pick_device(name_sub):
    for i, d in enumerate(sd.query_devices()):
        if d["max_output_channels"] > 0 and name_sub.lower() in d["name"].lower():
            return i, d
    raise SystemExit(f"no output device matching '{name_sub}'")


def load_wav_mono(path):
    with wave.open(path, "rb") as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise SystemExit(f"{path}: expected 16-bit, got sw={sw}")
    a = np.frombuffer(raw, dtype=np.int16).reshape(-1, ch)
    if ch > 1:
        a = a.mean(axis=1).astype(np.int16)
    return (a.astype(np.float32) / 32768.0), sr


class SerialLogger(threading.Thread):
    def __init__(self, port, baud, log_path, stop_event):
        super().__init__(daemon=True)
        self.port = port
        self.baud = baud
        self.log_path = log_path
        self.stop_event = stop_event
        self.lines = []
        self._buf = b""

    def run(self):
        with serial.Serial(self.port, self.baud, timeout=0.1) as s, open(self.log_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"=== LOG START {iso_now()} port={self.port} baud={self.baud} ===\n")
            f.flush()
            while not self.stop_event.is_set():
                data = s.read(256)
                if not data:
                    continue
                self._buf += data
                while b"\n" in self._buf:
                    line, _, self._buf = self._buf.partition(b"\n")
                    text = line.decode("ascii", errors="replace").rstrip("\r")
                    stamp = iso_now()
                    f.write(f"{stamp}\t{text}\n")
                    f.flush()
                    self.lines.append(text)
            if self._buf:
                text = self._buf.decode("ascii", errors="replace").rstrip("\r")
                f.write(f"{iso_now()}\t{text}\n")
                self.lines.append(text)


def analyze(lines):
    body = "\n".join(lines)
    n_err = len(ERR_PAT.findall(body))
    body_nocontrol = CTRL_PAT.sub("", body)
    body_noerr = ERR_PAT.sub("", body_nocontrol)
    total = max(len(body_noerr), 1)
    printable = sum(1 for c in body_noerr if 32 <= ord(c) < 127 or c in "\r\n\t ")
    return {
        "err_count": n_err,
        "total_chars": len(body_noerr),
        "printable_chars": printable,
        "printable_ratio": printable / total,
    }


def play_and_log(wav_path, outdir, device_idx, gain, port, baud, settle_ms, auto_center=True, prep_cmds=None, tag=None):
    af, sr = load_wav_mono(wav_path)
    af *= gain
    np.clip(af, -1.0, 1.0, out=af)
    dur = len(af) / sr
    name = Path(wav_path).stem
    log_name = f"{tag}_{name}" if tag else name
    log_path = Path(outdir) / f"{log_name}.log"

    prep = list(prep_cmds or [])
    center_info = None
    if auto_center:
        center_info = find_rtty_center(wav_path)
        if center_info:
            fc = int(round(center_info["center"]))
            prep.append(f"FREQ {fc}")
            prep.append("AFC ON")
    prep.append("CLEAR")
    if prep:
        send_cmds(port, baud, prep)

    stop = threading.Event()
    logger = SerialLogger(port, baud, str(log_path), stop)
    logger.start()
    time.sleep(settle_ms / 1000.0)
    t0 = time.time()
    sd.play(af, samplerate=sr, device=device_idx, blocking=True)
    t1 = time.time()
    time.sleep(settle_ms / 1000.0)
    stop.set()
    logger.join(timeout=3.0)
    stats = analyze(logger.lines)
    stats.update({"wav": wav_path, "duration_s": round(dur, 1), "wallclock_s": round(t1 - t0, 1), "log": str(log_path)})
    if center_info:
        stats["center"] = round(center_info["center"], 1)
        stats["mark"] = round(center_info["mark"], 1)
        stats["space"] = round(center_info["space"], 1)
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wavs", nargs="*", default=[])
    ap.add_argument("--wav-dir", default=None)
    ap.add_argument("--glob", default="*.WAV")
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--device", default="LEN Q27h-10")
    ap.add_argument("--port", default="COM27")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--gain", type=float, default=0.8)
    ap.add_argument("--settle-ms", type=int, default=500)
    ap.add_argument("--tag", default=None, help="tag prepended to summary filename")
    ap.add_argument("--no-auto-center", action="store_true", help="disable WAV spectrum analysis + FREQ pre-set")
    ap.add_argument("--prep-cmd", action="append", default=[], help="extra serial command to send before each playback (repeatable)")
    args = ap.parse_args()

    wavs = list(args.wavs)
    if args.wav_dir:
        wavs.extend(sorted(glob.glob(os.path.join(args.wav_dir, args.glob))))
    if not wavs:
        raise SystemExit("no WAVs specified")

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    dev_idx, dev_info = pick_device(args.device)
    print(f"[bench] device [{dev_idx}]: {dev_info['name']}  default_sr={int(dev_info['default_samplerate'])}")
    print(f"[bench] port={args.port}@{args.baud}  gain={args.gain}  {len(wavs)} file(s)")
    print(f"[bench] outdir={outdir}")

    results = []
    for i, w in enumerate(wavs, 1):
        print(f"\n[{i}/{len(wavs)}] {Path(w).name} ...")
        r = play_and_log(w, outdir, dev_idx, args.gain, args.port, args.baud, args.settle_ms,
                         auto_center=not args.no_auto_center, prep_cmds=args.prep_cmd,
                         tag=args.tag)
        ctr = f"  center={r.get('center','?')}Hz" if r.get('center') else ""
        print(f"      duration={r['duration_s']}s  wall={r['wallclock_s']}s{ctr}  "
              f"[ERR]={r['err_count']}  printable={r['printable_chars']}/{r['total_chars']} "
              f"({r['printable_ratio']*100:.1f}%)")
        results.append(r)

    tag = args.tag or dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    summary_path = outdir / f"summary_{tag}.md"
    total_err = sum(r["err_count"] for r in results)
    total_chars = sum(r["total_chars"] for r in results)
    total_printable = sum(r["printable_chars"] for r in results)
    with open(summary_path, "w", encoding="utf-8") as f:
        f.write(f"# Bench replay {tag}\n\n")
        f.write(f"device: {dev_info['name']}  gain={args.gain}  port={args.port}@{args.baud}\n\n")
        f.write("| WAV | dur(s) | center | [ERR] | printable | total | % |\n")
        f.write("|---|--:|--:|--:|--:|--:|--:|\n")
        for r in results:
            f.write(f"| {Path(r['wav']).name} | {r['duration_s']} | {r.get('center','?')} | "
                    f"{r['err_count']} | {r['printable_chars']} | {r['total_chars']} | "
                    f"{r['printable_ratio']*100:.1f} |\n")
        f.write(f"| **TOTAL** | | | **{total_err}** | **{total_printable}** | **{total_chars}** | "
                f"**{total_printable/max(total_chars,1)*100:.1f}** |\n")
    print(f"\n[bench] summary -> {summary_path}")


if __name__ == "__main__":
    main()
