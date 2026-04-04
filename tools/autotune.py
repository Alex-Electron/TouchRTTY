#!/usr/bin/env python3
"""
TouchRTTY Auto-Tuner — optimizes DSP parameters via serial interface.

Usage:
    python autotune.py [COM_PORT] [--measure-only] [--quick]

Algorithm:
    1. Connect to device, enable DIAG stream
    2. Collect baseline metrics (ERR%, SNR, DPLL freq error)
    3. Hill-climb each parameter (ALPHA, BW, SQ) independently
    4. For each candidate value, measure for N seconds
    5. Pick the combination with lowest ERR% and best SNR
    6. Save optimal settings to flash
"""

import serial
import sys
import time
import re
from dataclasses import dataclass, field
from typing import Optional

DEFAULT_PORT = "COM27"
BAUD_RATE = 115200

# Parameter search ranges and step sizes
PARAMS = {
    "ALPHA": {"min": 0.010, "max": 0.150, "step": 0.010, "fmt": ".4f", "default": 0.050},
    "BW":    {"min": 0.40,  "max": 1.80,  "step": 0.10,  "fmt": ".2f", "default": 1.00},
    "SQ":    {"min": 3.0,   "max": 15.0,  "step": 1.0,   "fmt": ".1f", "default": 8.0},
}

@dataclass
class Measurement:
    snr: float = 0.0
    sig: float = 0.0
    err: float = 100.0
    sq_state: str = "SHUT"
    agc_db: float = 0.0
    dpll_phase: float = 0.0
    dpll_ferr: float = 0.0
    mark_env: float = 0.0
    space_env: float = 0.0
    count: int = 0

    def quality_score(self) -> float:
        """Higher is better. Combines ERR%, SNR, and DPLL stability."""
        if self.count == 0:
            return -999.0
        # Primary: minimize error rate (weight: 5x)
        # Secondary: maximize SNR
        # Tertiary: minimize DPLL freq error (stability)
        err_penalty = self.err * 5.0
        snr_bonus = max(0, self.snr)
        ferr_penalty = abs(self.dpll_ferr) * 1000.0
        sq_bonus = 10.0 if self.sq_state == "OPEN" else 0.0
        return -err_penalty + snr_bonus - ferr_penalty + sq_bonus


class AutoTuner:
    def __init__(self, port: str, quick: bool = False):
        self.port = port
        self.ser: Optional[serial.Serial] = None
        self.measure_time = 3.0 if quick else 6.0  # seconds per measurement
        self.settle_time = 1.0  # seconds to let DSP settle after param change

    def connect(self):
        print(f"[*] Connecting to {self.port}...")
        self.ser = serial.Serial(self.port, BAUD_RATE, timeout=0.5)
        time.sleep(0.5)
        self.flush()
        print("[+] Connected.")

    def close(self):
        if self.ser:
            self.ser.close()

    def flush(self):
        """Drain any buffered data."""
        self.ser.reset_input_buffer()
        time.sleep(0.1)
        while self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)
            time.sleep(0.05)

    def send(self, cmd: str):
        """Send a command to the device."""
        self.ser.write(f"{cmd}\r\n".encode())
        time.sleep(0.1)
        # Read echo/response
        while self.ser.in_waiting:
            line = self.ser.readline().decode(errors='replace').strip()
            if line.startswith(">>"):
                print(f"    {line}")

    def read_diag_lines(self, duration: float) -> list:
        """Read diagnostic [D] lines for the given duration."""
        lines = []
        end_time = time.time() + duration
        while time.time() < end_time:
            if self.ser.in_waiting:
                raw = self.ser.readline()
                try:
                    line = raw.decode(errors='replace').strip()
                except:
                    continue
                if line.startswith("[D]"):
                    lines.append(line)
            else:
                time.sleep(0.05)
        return lines

    def parse_diag(self, line: str) -> Optional[Measurement]:
        """Parse a [D] diagnostic line into a Measurement."""
        m = Measurement()
        try:
            # [D] SNR=15.3 SIG=-28.5 ERR=12% SQ=OPEN AGC=+13dB PH=0.45 FE=0.002 M=0.234 S=0.198 ...
            parts = line.split()
            for p in parts:
                if p.startswith("SNR="):
                    m.snr = float(p[4:])
                elif p.startswith("SIG="):
                    m.sig = float(p[4:])
                elif p.startswith("ERR="):
                    m.err = float(p[4:].rstrip('%'))
                elif p.startswith("SQ="):
                    m.sq_state = p[3:]
                elif p.startswith("AGC="):
                    m.agc_db = float(p[4:].rstrip('dB'))
                elif p.startswith("PH="):
                    m.dpll_phase = float(p[3:])
                elif p.startswith("FE="):
                    m.dpll_ferr = float(p[3:])
                elif p.startswith("M="):
                    m.mark_env = float(p[2:])
                elif p.startswith("S=") and not p.startswith("SIG") and not p.startswith("SQ"):
                    m.space_env = float(p[2:])
            m.count = 1
            return m
        except (ValueError, IndexError):
            return None

    def measure(self, duration: float = None) -> Measurement:
        """Collect and average measurements over duration seconds."""
        if duration is None:
            duration = self.measure_time

        lines = self.read_diag_lines(duration)
        if not lines:
            print("    [!] No diagnostic data received")
            return Measurement()

        avg = Measurement()
        count = 0
        sq_open = 0
        for line in lines:
            m = self.parse_diag(line)
            if m:
                avg.snr += m.snr
                avg.sig += m.sig
                avg.err += m.err
                avg.agc_db += m.agc_db
                avg.dpll_ferr += m.dpll_ferr
                avg.mark_env += m.mark_env
                avg.space_env += m.space_env
                if m.sq_state == "OPEN":
                    sq_open += 1
                count += 1

        if count > 0:
            avg.snr /= count
            avg.sig /= count
            avg.err /= count
            avg.agc_db /= count
            avg.dpll_ferr /= count
            avg.mark_env /= count
            avg.space_env /= count
            avg.sq_state = "OPEN" if sq_open > count / 2 else "SHUT"
            avg.count = count

        return avg

    def set_param(self, name: str, value: float):
        """Set a parameter and wait for DSP to settle."""
        fmt = PARAMS[name]["fmt"]
        cmd = f"{name} {value:{fmt}}"
        self.send(cmd)
        self.flush()
        time.sleep(self.settle_time)
        self.flush()  # Clear any data during settle

    def print_measurement(self, m: Measurement, label: str = ""):
        prefix = f"  [{label}]" if label else "  "
        sq_icon = "O" if m.sq_state == "OPEN" else "X"
        print(f"{prefix} SNR={m.snr:.1f}dB SIG={m.sig:.1f}dB ERR={m.err:.0f}% "
              f"SQ={sq_icon} FE={m.dpll_ferr:.5f} Score={m.quality_score():.1f} "
              f"(n={m.count})")

    def optimize_param(self, name: str, current_best: dict) -> float:
        """Find optimal value for one parameter while others are fixed."""
        cfg = PARAMS[name]
        print(f"\n{'='*50}")
        print(f"[*] Optimizing {name} (range {cfg['min']}-{cfg['max']}, step {cfg['step']})")
        print(f"{'='*50}")

        best_val = current_best[name]
        best_score = -999.0
        best_measurement = None
        results = []

        val = cfg["min"]
        while val <= cfg["max"] + 0.001:
            self.set_param(name, val)
            m = self.measure()
            self.print_measurement(m, f"{name}={val:{cfg['fmt']}}")
            score = m.quality_score()
            results.append((val, score, m))

            if score > best_score:
                best_score = score
                best_val = val
                best_measurement = m

            val += cfg["step"]
            val = round(val, 4)

        print(f"\n  >>> Best {name} = {best_val:{cfg['fmt']}} "
              f"(Score={best_score:.1f}, ERR={best_measurement.err:.0f}%, "
              f"SNR={best_measurement.snr:.1f}dB)")
        return best_val

    def run_baseline(self) -> Measurement:
        """Measure current performance without changing anything."""
        print("\n[*] Measuring baseline (current settings)...")
        self.send("STATUS")
        time.sleep(0.5)
        while self.ser.in_waiting:
            print("   ", self.ser.readline().decode(errors='replace').strip())

        self.flush()
        m = self.measure(duration=self.measure_time)
        self.print_measurement(m, "BASELINE")
        return m

    def run(self, measure_only: bool = False):
        """Main auto-tuning loop."""
        self.connect()

        try:
            # Enable diagnostic stream
            self.send("DIAG ON")
            time.sleep(0.5)
            self.flush()

            # Baseline
            baseline = self.run_baseline()

            if measure_only:
                print("\n[*] Measure-only mode. Done.")
                self.send("DIAG OFF")
                return

            if baseline.sq_state == "SHUT":
                print("\n[!] WARNING: Squelch is SHUT — no signal detected!")
                print("[!] Make sure a valid RTTY signal is being fed to the device.")
                print("[!] Continuing anyway (SQ optimization may help)...\n")

            # Get current values as starting point
            current = {
                "ALPHA": PARAMS["ALPHA"]["default"],
                "BW": PARAMS["BW"]["default"],
                "SQ": PARAMS["SQ"]["default"],
            }

            # Phase 1: Coarse sweep each parameter independently
            print("\n" + "=" * 50)
            print("  PHASE 1: Independent Parameter Sweep")
            print("=" * 50)

            for param in ["ALPHA", "BW", "SQ"]:
                best = self.optimize_param(param, current)
                current[param] = best
                # Apply the best value found so far
                self.set_param(param, best)

            # Phase 2: Fine-tune around best values
            print("\n" + "=" * 50)
            print("  PHASE 2: Fine-Tuning")
            print("=" * 50)

            for param in ["ALPHA", "BW"]:
                cfg = PARAMS[param]
                fine_step = cfg["step"] / 2
                center = current[param]
                fine_min = max(cfg["min"], center - cfg["step"] * 2)
                fine_max = min(cfg["max"], center + cfg["step"] * 2)

                print(f"\n[*] Fine-tuning {param} around {center:{cfg['fmt']}}")

                best_val = center
                best_score = -999.0

                val = fine_min
                while val <= fine_max + 0.001:
                    self.set_param(param, val)
                    m = self.measure()
                    self.print_measurement(m, f"{param}={val:{cfg['fmt']}}")
                    if m.quality_score() > best_score:
                        best_score = m.quality_score()
                        best_val = val
                    val += fine_step
                    val = round(val, 4)

                current[param] = best_val
                self.set_param(param, best_val)
                print(f"  >>> Fine-tuned {param} = {best_val:{cfg['fmt']}}")

            # Apply final values and measure
            print("\n" + "=" * 50)
            print("  FINAL RESULTS")
            print("=" * 50)

            for param, val in current.items():
                self.set_param(param, val)

            time.sleep(2.0)
            self.flush()
            final = self.measure(duration=self.measure_time * 2)

            print(f"\n  Optimal parameters:")
            print(f"    ALPHA = {current['ALPHA']:{PARAMS['ALPHA']['fmt']}}")
            print(f"    BW    = {current['BW']:{PARAMS['BW']['fmt']}}")
            print(f"    SQ    = {current['SQ']:{PARAMS['SQ']['fmt']}}")
            self.print_measurement(final, "FINAL")
            self.print_measurement(baseline, "WAS  ")

            improvement = baseline.err - final.err
            if improvement > 0:
                print(f"\n  ERR improved by {improvement:.0f}% points!")
            elif improvement < 0:
                print(f"\n  [!] ERR worsened by {-improvement:.0f}% — reverting not implemented, check manually")

            # Save
            print("\n[*] Saving optimal settings to flash...")
            self.send("SAVE")
            time.sleep(1.0)

            self.send("DIAG OFF")
            print("[+] Auto-tuning complete!")

        except KeyboardInterrupt:
            print("\n[!] Interrupted by user")
            self.send("DIAG OFF")
        finally:
            self.close()


def main():
    port = DEFAULT_PORT
    measure_only = False
    quick = False

    for arg in sys.argv[1:]:
        if arg == "--measure-only":
            measure_only = True
        elif arg == "--quick":
            quick = True
        elif not arg.startswith("-"):
            port = arg

    tuner = AutoTuner(port, quick=quick)
    tuner.run(measure_only=measure_only)


if __name__ == "__main__":
    main()
