#!/usr/bin/env python3
import argparse
import re
import sys


def compile_source(src: str) -> str:
    # Extremely small subset:
    #   int main(){ return <int>; }
    #   putc(<int>);  // emits OUT to UART
    # Produces VM32 asm text.
    out = []
    tokens = [line.strip() for line in src.splitlines() if line.strip()]
    for line in tokens:
        m = re.match(r"return\s+([0-9]+)\s*;", line)
        if m:
            val = int(m.group(1))
            out.append(f"PUSH {val}")
            out.append("HALT")
            continue
        m = re.match(r"putc\(\s*([0-9]+)\s*\)\s*;", line)
        if m:
            val = int(m.group(1))
            out.append(f"PUSH {val}")
            out.append("PUSH 0x0FF0")
            out.append("OUT")
            continue
    if not out:
        raise ValueError("unsupported source (try: putc(65); or return 42;)")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description="VM32 tiny C translator (very small subset)")
    ap.add_argument("input", help="input .c file")
    ap.add_argument("-o", "--output", help="output .asm file (default stdout)")
    args = ap.parse_args()

    with open(args.input, "r", encoding="utf-8") as f:
        src = f.read()

    try:
        asm = compile_source(src)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(asm)
    else:
        sys.stdout.write(asm)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
