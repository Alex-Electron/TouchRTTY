#!/usr/bin/env python3
"""send_serial_cmd.py - send one or more CRLF-terminated commands to a serial port."""
import argparse
import time
import serial

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='COM27')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--cmd', action='append', required=True)
    ap.add_argument('--delay', type=float, default=0.3)
    args = ap.parse_args()
    with serial.Serial(args.port, args.baud, timeout=1) as s:
        for c in args.cmd:
            line = c + '\r\n'
            s.write(line.encode('ascii'))
            s.flush()
            time.sleep(args.delay)
            resp = s.read(4096)
            try:
                print(f">>> {c}\n{resp.decode('ascii', errors='replace')}")
            except Exception:
                print(f">>> {c}\n{resp!r}")

if __name__ == '__main__':
    main()
