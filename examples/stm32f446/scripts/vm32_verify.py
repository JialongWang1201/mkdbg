#!/usr/bin/env python3
import argparse
import json
import sys
from dataclasses import dataclass


OPCODE_LEN = {
    0x00: 1,  # NOP
    0x01: 5,  # PUSH imm32
    0x02: 1,  # DUP
    0x03: 1,  # DROP
    0x04: 1,  # SWAP
    0x05: 1,  # OVER
    0x10: 1,  # ADD
    0x11: 1,  # SUB
    0x12: 1,  # AND
    0x13: 1,  # OR
    0x14: 1,  # XOR
    0x15: 1,  # NOT
    0x16: 1,  # SHL
    0x17: 1,  # SHR
    0x20: 1,  # LOAD
    0x21: 1,  # STORE
    0x30: 2,  # JZ rel8
    0x31: 2,  # CALL rel8
    0x32: 1,  # RET
    0x40: 1,  # IN
    0x41: 1,  # OUT
    0xFF: 1,  # HALT
}


@dataclass
class VerifyResult:
    ok: bool
    reason: str
    entry: int
    span: int
    reachable: int
    max_steps: int
    reject_pc: int
    reject_op: int


def s8(v: int) -> int:
    return v - 256 if v > 127 else v


def load_blob(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def emit_error_json(reason: str, detail: str, input_path: str) -> None:
    print(json.dumps({
        "ok": False,
        "reason": reason,
        "detail": detail,
        "input": input_path,
        "entry": 0,
        "span": 0,
        "reachable": 0,
        "max_steps": 0,
        "reject_pc": 0,
        "reject_op": 0,
    }))


def verify_bounded_cfg(data: bytes, entry: int, span: int) -> VerifyResult:
    res = VerifyResult(
        ok=False,
        reason="bad_arg",
        entry=entry,
        span=span,
        reachable=0,
        max_steps=0,
        reject_pc=0,
        reject_op=0,
    )

    if span <= 0 or entry < 0 or entry >= len(data) or (entry + span) > len(data):
        return res

    lo = entry
    hi = entry + span
    queue = [entry]
    seen = set([entry])

    while queue:
        pc = queue.pop(0)
        op = data[pc]
        ilen = OPCODE_LEN.get(op, 0)
        nxt = pc + ilen

        if ilen == 0:
            res.reason = "illegal_op"
            res.reject_pc = pc
            res.reject_op = op
            return res
        if pc < lo or nxt > hi:
            res.reason = "decode_oob"
            res.reject_pc = pc
            res.reject_op = op
            return res
        if op in (0x31, 0x32):
            res.reason = "call_ret_forbidden"
            res.reject_pc = pc
            res.reject_op = op
            return res

        succ = []
        if op == 0xFF:
            succ = []
        elif op == 0x30:
            rel = s8(data[pc + 1])
            tgt = nxt + rel
            if nxt >= hi or tgt < lo or tgt >= hi:
                res.reason = "target_oob"
                res.reject_pc = pc
                res.reject_op = op
                return res
            succ = [nxt, tgt]
        else:
            if nxt >= hi:
                res.reason = "target_oob"
                res.reject_pc = pc
                res.reject_op = op
                return res
            succ = [nxt]

        for s in succ:
            if s <= pc:
                res.reason = "back_edge"
                res.reject_pc = pc
                res.reject_op = op
                return res
            if s not in seen:
                seen.add(s)
                queue.append(s)

    res.ok = True
    res.reason = "ok"
    res.reachable = len(seen)
    res.max_steps = len(seen)
    return res


def main() -> int:
    ap = argparse.ArgumentParser(description="VM32 bounded CFG verifier")
    ap.add_argument("input", help="input .bin file")
    ap.add_argument("--entry", default="0", help="entry offset within input")
    ap.add_argument("--span", default="0", help="window size; default len(input)-entry")
    ap.add_argument("--json", action="store_true", help="print JSON result")
    args = ap.parse_args()

    try:
        data = load_blob(args.input)
    except OSError as e:
        if args.json:
            emit_error_json("input_read", str(e), args.input)
        else:
            print(f"verify: error reason=input_read detail={e}", file=sys.stderr)
        return 2

    try:
        entry = int(args.entry, 0)
    except ValueError:
        if args.json:
            emit_error_json("bad_entry", args.entry, args.input)
        else:
            print(f"verify: error reason=bad_entry detail={args.entry}", file=sys.stderr)
        return 2

    try:
        span = int(args.span, 0)
    except ValueError:
        if args.json:
            emit_error_json("bad_span", args.span, args.input)
        else:
            print(f"verify: error reason=bad_span detail={args.span}", file=sys.stderr)
        return 2

    if span == 0 and 0 <= entry < len(data):
        span = len(data) - entry

    res = verify_bounded_cfg(data, entry, span)
    if args.json:
        print(json.dumps({
            "ok": res.ok,
            "reason": res.reason,
            "entry": res.entry,
            "span": res.span,
            "reachable": res.reachable,
            "max_steps": res.max_steps,
            "reject_pc": res.reject_pc,
            "reject_op": res.reject_op,
        }))
    else:
        if res.ok:
            print(f"verify: ok reason={res.reason} max_steps={res.max_steps} reachable={res.reachable}")
        else:
            print(f"verify: reject reason={res.reason} pc=0x{res.reject_pc:04X} op=0x{res.reject_op:02X}",
                  file=sys.stderr)
    if res.ok:
        return 0
    if res.reason == "bad_arg":
        return 2
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
