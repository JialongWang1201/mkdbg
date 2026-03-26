#!/usr/bin/env python3
import argparse
import sys


OPCODES = {
    0x00: "NOP",
    0x01: "PUSH",
    0x02: "DUP",
    0x03: "DROP",
    0x04: "SWAP",
    0x05: "OVER",
    0x10: "ADD",
    0x11: "SUB",
    0x12: "AND",
    0x13: "OR",
    0x14: "XOR",
    0x15: "NOT",
    0x16: "SHL",
    0x17: "SHR",
    0x20: "LOAD",
    0x21: "STORE",
    0x30: "JZ",
    0x31: "CALL",
    0x32: "RET",
    0x40: "IN",
    0x41: "OUT",
    0xFF: "HALT",
}


def load_bytes(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def u32le(buf, idx):
    return buf[idx] | (buf[idx + 1] << 8) | (buf[idx + 2] << 16) | (buf[idx + 3] << 24)


def s8(v):
    return v - 256 if v > 127 else v


def disasm(data: bytes, base: int):
    out = []
    pc = 0
    while pc < len(data):
        addr = base + pc
        op = data[pc]
        mnem = OPCODES.get(op)
        if mnem is None:
            out.append(f"{addr:04X}: .byte 0x{op:02X}")
            pc += 1
            continue
        if mnem == "PUSH":
            if pc + 5 > len(data):
                out.append(f"{addr:04X}: PUSH <truncated>")
                break
            imm = u32le(data, pc + 1)
            out.append(f"{addr:04X}: PUSH 0x{imm:08X}")
            pc += 5
            continue
        if mnem in ("JZ", "CALL"):
            if pc + 2 > len(data):
                out.append(f"{addr:04X}: {mnem} <truncated>")
                break
            rel = s8(data[pc + 1])
            target = addr + 2 + rel
            out.append(f"{addr:04X}: {mnem} 0x{target:04X}")
            pc += 2
            continue
        out.append(f"{addr:04X}: {mnem}")
        pc += 1
    return out


def main():
    ap = argparse.ArgumentParser(description="VM32 disassembler")
    ap.add_argument("input", help="input .bin file")
    ap.add_argument("--base", default="0", help="base address")
    args = ap.parse_args()

    data = load_bytes(args.input)
    base = int(args.base, 0)
    for line in disasm(data, base):
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
