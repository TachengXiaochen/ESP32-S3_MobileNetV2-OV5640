#!/usr/bin/env python3
"""Capture ESP32 serial output to a log file (no TTY required)."""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture UART log from ESP32")
    parser.add_argument("--port", default="COM7")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        print("pyserial not installed. Run: pip install pyserial", file=sys.stderr)
        return 1

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    lines: list[str] = []
    header = (
        f"# capture started {datetime.now().isoformat(timespec='seconds')}\n"
        f"# port={args.port} baud={args.baud} seconds={args.seconds}\n"
    )

    try:
        with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
            deadline = time.time() + args.seconds
            while time.time() < deadline:
                chunk = ser.read(4096)
                if chunk:
                    text = chunk.decode("utf-8", errors="replace")
                    lines.append(text)
                    sys.stdout.write(text)
                    sys.stdout.flush()
                else:
                    time.sleep(0.05)
    except serial.SerialException as exc:
        body = f"\n# ERROR: {exc}\n"
        out_path.write_text(header + body, encoding="utf-8")
        print(body, file=sys.stderr)
        return 2

    out_path.write_text(header + "".join(lines), encoding="utf-8")
    print(f"\n# saved {len(''.join(lines))} chars -> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
