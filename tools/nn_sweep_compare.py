"""nn_sweep_compare.py - run AWGN sweep twice (NN OFF, NN ON) and compare CER.

Plays a synthetic RTTY+AWGN sweep through LEN Q27h-10 (decoder hears it),
captures COM27 output, then computes per-SNR CER for each run via
cer_analyze.py. Output: a side-by-side table.

Designed for one-shot, autonomous execution: configures decoder via serial,
runs sweep_runner as subprocess in parallel with serial capture, repeats
twice with NN OFF vs ON.
"""
from __future__ import annotations
import argparse
import re
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

import serial


ROOT = Path(__file__).resolve().parents[1]
SWEEP_RUNNER = ROOT / "tools" / "sweep_runner.py"
CER_ANALYZE  = ROOT / "tools" / "cer_analyze.py"
DEFAULT_PORT = "COM27"
DEFAULT_DEVICE = "LEN Q27h-10"
DEFAULT_GT = "RYRYRY THE QUICK BROWN FOX JUMPS OVER 1234567890\r\n"


def configure(port: str, nn_on: bool, freq: int = 1500) -> None:
    """Pin decoder: 45.45 Bd, 170 Hz shift, FREQ <freq>, NOR, PATH HYB, NN as requested."""
    cmds = [
        "BAUD 0", "SHIFT 1", "INV NOR", f"FREQ {int(freq)}",
        "AFC ON", "PATH HYB",
        "NN ON" if nn_on else "NN OFF",
    ]
    with serial.Serial(port, 115200, timeout=0.5) as s:
        time.sleep(0.3)
        s.reset_input_buffer()
        for c in cmds:
            s.write((c + "\r\n").encode())
            s.flush()
            time.sleep(0.2)
            s.read_all()


def sweep_run(label: str, device: str, port: str, snr_from: float, snr_to: float,
              step: float, dwell: float, out_dir: Path,
              center: float | None = None,
              sig_level: float | None = None,
              seed: int = 42) -> tuple[Path, Path]:
    """Run one sweep, capture serial output. Returns (sweep_log, serial_log) paths."""
    out_dir.mkdir(parents=True, exist_ok=True)
    sweep_log = out_dir / f"{label}.sweep.txt"
    serial_log = out_dir / f"{label}.serial.txt"

    # Start serial capture thread first so we don't miss anything.
    stop_evt = threading.Event()
    lines: list[str] = []

    def serial_thread():
        with serial.Serial(port, 115200, timeout=0.1) as s:
            s.reset_input_buffer()
            buf = b""
            while not stop_evt.is_set():
                chunk = s.read(512)
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        ts = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
                        decoded = line.decode("utf-8", errors="replace").rstrip("\r")
                        if decoded:
                            lines.append(f"{ts}\t{decoded}")

    t = threading.Thread(target=serial_thread, daemon=True)
    t.start()
    print(f"[orch] {label}: serial capture started", flush=True)

    # Run sweep_runner as subprocess; it generates its own sweep-log.
    cmd = [
        sys.executable, str(SWEEP_RUNNER),
        "--device", device,
        "--from", str(snr_from), "--to", str(snr_to),
        "--step", str(step), "--dwell", str(dwell),
        "--out-sweep", str(sweep_log),
        "--seed", str(seed),  # reproducibility across NN ON vs OFF runs (same in both passes)
    ]
    if center is not None:
        cmd += ["--center", str(center)]
    if sig_level is not None:
        cmd += ["--sig-level", str(sig_level)]
    print(f"[orch] {label}: running sweep ({snr_from:+g}..{snr_to:+g} dB step {step} "
          f"dwell {dwell}s)...", flush=True)
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace")
        for line in proc.stdout:
            if any(k in line for k in ("[runner]", "SNR=", "[progress]", "===")):
                safe = line.rstrip().encode("ascii", "replace").decode("ascii")
                print(f"  {safe}", flush=True)
        proc.wait()
    finally:
        # 2-second drain after sweep ends so trailing chars land
        time.sleep(2.0)
        stop_evt.set()
        t.join(timeout=2.0)

    serial_log.write_text("\n".join(lines), encoding="utf-8")
    print(f"[orch] {label}: captured {len(lines)} serial lines", flush=True)
    return sweep_log, serial_log


def analyze(sweep_log: Path, serial_log: Path, gt: str) -> str:
    """Run cer_analyze.py and return its stdout text."""
    cmd = [
        sys.executable, str(CER_ANALYZE),
        "--sweep", str(sweep_log),
        "--serial", str(serial_log),
        "--gt", gt,
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    return res.stdout + ("\n--STDERR--\n" + res.stderr if res.stderr.strip() else "")


def parse_cer(text: str) -> dict[float, float]:
    """Pull SNR -> CER% from cer_analyze output (best-effort, handles both formats)."""
    out = {}
    re_with_path = re.compile(r"\s*\d+\s+(-?\d+)\s+\S+\s+\d+\s+([\d.]+)%")
    re_no_path   = re.compile(r"\s*\d+\s+(-?\d+)\s+\d+\s+([\d.]+)%")
    for line in text.splitlines():
        m = re_with_path.match(line) or re_no_path.match(line)
        if m:
            snr = int(m.group(1))
            cer = float(m.group(-1) if m.lastindex == 3 else m.group(2))
            out[snr] = cer
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port",   default=DEFAULT_PORT)
    ap.add_argument("--device", default=DEFAULT_DEVICE)
    ap.add_argument("--from",   dest="snr_from", type=float, default=4)
    ap.add_argument("--to",     dest="snr_to",   type=float, default=-16)
    ap.add_argument("--step",   type=float, default=4)
    ap.add_argument("--dwell",  type=float, default=30.0)
    ap.add_argument("--gt",     default=DEFAULT_GT)
    ap.add_argument("--seed",      type=int, default=42,
                    help="sweep_runner seed (same in NN OFF and ON passes for fair compare)")
    ap.add_argument("--center",    type=float, default=None,
                    help="center freq override (passes to sweep_runner)")
    ap.add_argument("--sig-level", type=float, default=None,
                    help="signal peak amplitude override (passes to sweep_runner)")
    ap.add_argument("--out-dir", default="datasets/logs/nn_compare",
                    help="directory for sweep+serial logs")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    results = {}
    for label, nn_on in [("nn_off", False), ("nn_on", True)]:
        print(f"\n{'='*60}\n[orch] === {label.upper()} ===\n{'='*60}", flush=True)
        configure(args.port, nn_on,
                  freq=int(args.center) if args.center else 1500)
        time.sleep(1.0)
        sweep_log, serial_log = sweep_run(
            label, args.device, args.port,
            args.snr_from, args.snr_to, args.step, args.dwell, out_dir,
            center=args.center, sig_level=args.sig_level, seed=args.seed,
        )
        print(f"[orch] {label}: analyzing...", flush=True)
        rep = analyze(sweep_log, serial_log, args.gt)
        (out_dir / f"{label}.report.txt").write_text(rep, encoding="utf-8")
        cer_map = parse_cer(rep)
        results[label] = cer_map
        print(f"[orch] {label}: per-SNR CER = {cer_map}", flush=True)

    # Comparison table
    snrs = sorted({s for m in results.values() for s in m}, reverse=True)
    print(f"\n{'='*60}\nNN ON vs OFF — CER per SNR\n{'='*60}", flush=True)
    print(f"{'SNR (dB)':>10s}  {'NN OFF':>10s}  {'NN ON':>10s}  {'delta':>10s}")
    print("-" * 50)
    delta_lines = []
    for snr in snrs:
        off = results.get("nn_off", {}).get(snr)
        on  = results.get("nn_on", {}).get(snr)
        off_s = f"{off:.2f}%" if off is not None else "—"
        on_s  = f"{on:.2f}%"  if on  is not None else "—"
        if off is not None and on is not None:
            delta = on - off
            delta_s = f"{delta:+.2f}%"
        else:
            delta_s = "—"
        line = f"{snr:>10d}  {off_s:>10s}  {on_s:>10s}  {delta_s:>10s}"
        print(line)
        delta_lines.append(line)
    (out_dir / "compare.txt").write_text(
        "SNR (dB)  NN OFF  NN ON  delta\n" + "\n".join(delta_lines) + "\n",
        encoding="utf-8")
    print(f"\nSaved: {out_dir}/compare.txt", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
