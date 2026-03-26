#!/usr/bin/env python3
import argparse
import sys


def load_bytes(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def build_lines(data: bytes, base: int, chunk: int):
    lines = []
    for i in range(0, len(data), chunk):
        part = data[i:i + chunk]
        items = ["vm", "load", str(base + i)]
        items.extend(str(b) for b in part)
        lines.append(" ".join(items) + "\r\n")
    return lines


def main():
    ap = argparse.ArgumentParser(description="VM32 UART loader")
    ap.add_argument("input", help="input .bin file")
    ap.add_argument("--port", help="serial port, e.g. /dev/tty.usbmodemXXXX")
    ap.add_argument("--baud", type=int, default=115200, help="baud rate")
    ap.add_argument("--base", default="0", help="base address")
    ap.add_argument("--chunk", type=int, default=32, help="bytes per load line")
    ap.add_argument("--dry-run", action="store_true", help="print commands only")
    args = ap.parse_args()

    data = load_bytes(args.input)
    base = int(args.base, 0)
    lines = build_lines(data, base, args.chunk)

    if args.dry_run or not args.port:
        for line in lines:
            sys.stdout.write(line)
        return 0

    try:
        import serial  # type: ignore
    except Exception:
        print("error: pyserial is required for UART load; use --dry-run", file=sys.stderr)
        return 1

    with serial.Serial(args.port, args.baud, timeout=1) as ser:
        for line in lines:
            ser.write(line.encode("ascii"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
