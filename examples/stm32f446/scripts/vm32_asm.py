#!/usr/bin/env python3
import argparse
import re
import sys

OPCODES = {
    "NOP": 0x00,
    "PUSH": 0x01,
    "DUP": 0x02,
    "DROP": 0x03,
    "SWAP": 0x04,
    "OVER": 0x05,
    "ADD": 0x10,
    "SUB": 0x11,
    "AND": 0x12,
    "OR": 0x13,
    "XOR": 0x14,
    "NOT": 0x15,
    "SHL": 0x16,
    "SHR": 0x17,
    "LOAD": 0x20,
    "STORE": 0x21,
    "JZ": 0x30,
    "CALL": 0x31,
    "RET": 0x32,
    "IN": 0x40,
    "OUT": 0x41,
    "HALT": 0xFF,
}

LINE_RE = re.compile(r"[;\#].*$")


def parse_int(tok: str) -> int:
    return int(tok, 0)


def instr_size(mnemonic: str) -> int:
    if mnemonic == "PUSH":
        return 1 + 4
    if mnemonic in ("JZ", "CALL"):
        return 1 + 1
    return 1


def encode_push(val: int) -> bytes:
    return bytes([OPCODES["PUSH"],
                  val & 0xFF,
                  (val >> 8) & 0xFF,
                  (val >> 16) & 0xFF,
                  (val >> 24) & 0xFF])


def encode_rel8(op: str, target: int, pc_after: int) -> bytes:
    rel = target - pc_after
    if rel < -128 or rel > 127:
        raise ValueError(f"{op} target out of range: rel={rel}")
    return bytes([OPCODES[op], rel & 0xFF])


def strip_line(line: str) -> str:
    line = LINE_RE.sub("", line)
    return line.strip()


def tokenize(line: str):
    return [t for t in re.split(r"\s+|,", line) if t]


def assemble(lines):
    labels = {}
    pc = 0
    cleaned = []

    # First pass: resolve labels and sizes
    for raw in lines:
        line = strip_line(raw)
        if not line:
            continue
        if line.endswith(":"):
            labels[line[:-1]] = pc
            continue
        if ":" in line:
            label, rest = line.split(":", 1)
            labels[label.strip()] = pc
            line = rest.strip()
            if not line:
                continue
        toks = tokenize(line)
        if not toks:
            continue
        cleaned.append((pc, toks))
        head = toks[0].upper()
        if head == ".ORG":
            pc = parse_int(toks[1])
            continue
        if head == ".BYTE":
            pc += len(toks) - 1
            continue
        if head == ".WORD":
            pc += 4 * (len(toks) - 1)
            continue
        pc += instr_size(head)

    # Second pass: encode
    output = bytearray()
    pc = 0
    for addr, toks in cleaned:
        head = toks[0].upper()
        if head == ".ORG":
            new_pc = parse_int(toks[1])
            if new_pc < pc:
                raise ValueError("ORG cannot move backwards")
            output.extend(b"\x00" * (new_pc - pc))
            pc = new_pc
            continue
        if head == ".BYTE":
            for t in toks[1:]:
                output.append(parse_int(t) & 0xFF)
                pc += 1
            continue
        if head == ".WORD":
            for t in toks[1:]:
                v = parse_int(t)
                output.extend(encode_push(v)[1:])  # 4 bytes
                pc += 4
            continue

        if head not in OPCODES:
            raise ValueError(f"Unknown opcode: {head}")

        if head == "PUSH":
            v = toks[1]
            val = labels.get(v, None)
            if val is None:
                val = parse_int(v)
            output.extend(encode_push(val))
            pc += 5
            continue

        if head in ("JZ", "CALL"):
            target_tok = toks[1]
            target = labels.get(target_tok, None)
            if target is None:
                target = parse_int(target_tok)
            pc_after = pc + 2
            output.extend(encode_rel8(head, target, pc_after))
            pc += 2
            continue

        output.append(OPCODES[head])
        pc += 1

    return bytes(output)


def main():
    ap = argparse.ArgumentParser(description="VM32 assembler")
    ap.add_argument("input", help="input .asm file")
    ap.add_argument("-o", "--output", help="output .bin file")
    ap.add_argument("--load", action="store_true", help="print vm load command")
    ap.add_argument("--base", default="0", help="base address for vm load")
    args = ap.parse_args()

    with open(args.input, "r", encoding="utf-8") as f:
        lines = f.readlines()

    try:
        blob = assemble(lines)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.output:
        with open(args.output, "wb") as f:
            f.write(blob)

    if args.load:
        base = int(args.base, 0)
        parts = ["vm", "load", str(base)]
        parts.extend(str(b) for b in blob)
        print(" ".join(parts))

    if not args.output and not args.load:
        # default to hex dump
        print(" ".join(f"{b:02X}" for b in blob))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
