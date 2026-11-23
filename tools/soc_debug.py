#!/usr/bin/env python3
"""Inspect a declared SoC through an existing local OpenOCD session."""

import argparse
import json
import re
import socket
import subprocess
import time
from pathlib import Path

from board_evidence import integer, load_object, require


def identifier(value):
    require(isinstance(value, str) and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.-]*", value),
            f"invalid target or profile identifier: {value!r}")
    return value


def port(value, name):
    integer(value, name, 1)
    require(value <= 65535, f"{name}: port exceeds 65535")
    return value


def address(value):
    number = int(value, 0) if isinstance(value, str) else value
    integer(number, "address")
    require(number < 2 ** 64 and number % 4 == 0, "address must be an aligned 64-bit address")
    return number


def validate(profile):
    require(profile["version"] == 1, "unsupported SoC profile version")
    for key in ("soc", "revision"):
        require(isinstance(profile[key], str) and profile[key], f"{key}: expected a name")
    port(profile["tcl_port"], "tcl_port")
    cores = profile["cores"]
    require(isinstance(cores, dict) and 0 < len(cores) <= 32, "expected 1..32 cores")
    targets, gdb_ports = set(), set()
    for name, core in cores.items():
        identifier(name)
        target = identifier(core["target"])
        gdb_port = port(core["gdb_port"], f"{name}.gdb_port")
        require(target not in targets and gdb_port not in gdb_ports,
                f"{name}: duplicate target or GDB endpoint")
        targets.add(target)
        gdb_ports.add(gdb_port)
        for domain in ("power", "reset"):
            require(core[f"{domain}_domain"] in profile[f"{domain}_domains"],
                    f"{name}: unknown {domain} domain")

    spaces = profile["address_spaces"]
    for name, space in spaces.items():
        identifier(name)
        require(space["core"] in cores, f"{name}: unknown core")
        base = address(space["base"])
        size = integer(space["size"], f"{name}.size", 4)
        require(size % 4 == 0 and base + size <= 2 ** 64, f"{name}: invalid range")
    require(len(profile["reads"]) <= 64, "expected at most 64 read ranges")
    for name, region in profile["reads"].items():
        identifier(name)
        space = spaces[region["space"]]
        offset = address(region["offset"])
        words = integer(region["words"], f"{name}.words", 1)
        require(words <= 256 and offset + words * 4 <= space["size"], f"{name}: read exceeds range")
        require(region["kind"] in ("status", "error", "trace"), f"{name}: invalid read kind")
        require(type(region["requires_halt"]) is bool, f"{name}: requires_halt must be boolean")
        fields = region.get("fields", {})
        require(isinstance(fields, dict) and len(fields) <= 64, f"{name}: expected up to 64 fields")
        for field_name, field in fields.items():
            identifier(field_name)
            word = integer(field["word"], f"{name}.{field_name}.word")
            lsb = integer(field["lsb"], f"{name}.{field_name}.lsb")
            width = integer(field["width"], f"{name}.{field_name}.width", 1)
            require(word < words and lsb + width <= 32, f"{name}.{field_name}: field exceeds read")
    for name, group in profile["halt_groups"].items():
        identifier(name)
        require(isinstance(group, list) and group and len(set(group)) == len(group)
                and all(core in cores for core in group), f"{name}: invalid halt group")


def rpc(tcl_port, script, timeout):
    deadline = time.monotonic() + timeout
    with socket.create_connection(("127.0.0.1", tcl_port), timeout) as connection:
        connection.sendall(script.encode("ascii") + b"\x1a")
        response = bytearray()
        while len(response) < 512 * 1024:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("OpenOCD RPC deadline expired")
            connection.settimeout(remaining)
            chunk = connection.recv(8192)
            if not chunk:
                raise OSError("OpenOCD disconnected before returning a result")
            data, marker, _ = chunk.partition(b"\x1a")
            response.extend(data)
            if marker:
                return response.decode("ascii")
        raise ValueError("OpenOCD reply exceeds limit")


def snapshot_script(profile, group):
    cores = profile["cores"]
    lines = ["set mkdbg_cleanup {}", "set mkdbg_output {}"]
    if group:
        lines.append("set mkdbg_current [target current]")
    lines.append("set mkdbg_failed [catch {")
    for name, core in cores.items():
        target = core["target"]
        lines += [f"set mkdbg_state({name}) unavailable",
                  f"if {{[{target} was_examined] && ![catch {{{target} arp_poll}}]}} {{",
                  f"  set mkdbg_state({name}) [{target} curstate]", "}",
                  f'append mkdbg_output "state {name} $mkdbg_state({name})\\n"']
    if group:
        for name in group:
            lines += [f'if {{$mkdbg_state({name}) ni {{running halted}}}} {{error "state unavailable for {name}"}}']
        for name in group:
            target = cores[name]["target"]
            lines += [f'if {{$mkdbg_state({name}) eq "running"}} {{',
                      f"  lappend mkdbg_cleanup {target}", f"  {target} arp_halt", "}"]
        for name in group:
            lines += [f"{cores[name]['target']} arp_waitstate halted 500"]
    for name, region in profile["reads"].items():
        space = profile["address_spaces"][region["space"]]
        core = space["core"]
        target = cores[core]["target"]
        addr = address(space["base"]) + address(region["offset"])
        condition = f'$mkdbg_state({core}) eq "halted"' if region["requires_halt"] else (
            f'$mkdbg_state({core}) in {{running halted}}')
        if core in group:
            condition = "1"
        lines += [
            f"if {{{condition}}} {{",
            f"  if {{[catch {{{target} read_memory 0x{addr:x} 32 {region['words']} phys}} mkdbg_words]}} {{",
            f'    append mkdbg_output "read-failed {name}\\n"',
            "  } else {",
            f'    append mkdbg_output "read {name} $mkdbg_words\\n"',
            "  }",
            "} else {",
            f'  append mkdbg_output "read-skipped {name}\\n"',
            "}",
        ]
    lines += [
        "} mkdbg_error]",
        "foreach mkdbg_target $mkdbg_cleanup {",
        "  if {[catch {targets $mkdbg_target; resume}]} {",
        '    append mkdbg_output "resume-failed $mkdbg_target\\n"',
        "  }",
        "}",
    ]
    if group:
        lines.append('if {[catch {targets $mkdbg_current}]} {append mkdbg_output "target-restore-failed\\n"}')
    lines += [
        'if {$mkdbg_failed} {append mkdbg_output "operation-failed $mkdbg_error\\n"}',
        'append mkdbg_output "end\\n"',
        "set mkdbg_output",
    ]
    return "\n".join(lines)


def snapshot(profile, group, clock):
    start = time.monotonic_ns()
    timeout = 5 + len(group)
    try:
        reply = rpc(profile["tcl_port"], snapshot_script(profile, group), timeout)
    except OSError as exc:
        if group:
            raise OSError(f"{exc}; verify run state of {', '.join(group)} in OpenOCD") from exc
        raise
    end = time.monotonic_ns()
    rows = reply.splitlines()
    require(bool(rows) and rows[-1] == "end", "incomplete OpenOCD snapshot; verify target state")
    cores, reads, failures = {}, {}, []
    for row in rows[:-1]:
        fields = row.split()
        require(bool(fields), "empty OpenOCD response row")
        tag, *values = fields
        if tag == "state":
            name, state = values
            require(name in profile["cores"] and name not in cores, "unexpected core reply")
            cores[name] = {**profile["cores"][name], "state": state}
        elif tag in ("read", "read-failed", "read-skipped"):
            name, *words = values
            require(name in profile["reads"] and name not in reads, "unexpected read reply")
            spec = profile["reads"][name]
            entry = {**spec, "status": tag}
            if tag == "read":
                numbers = [int(value, 0) for value in words]
                require(len(numbers) == spec["words"] and all(0 <= n <= 0xffffffff for n in numbers),
                        f"{name}: invalid OpenOCD word count or value")
                entry["data"] = numbers
                if spec.get("fields"):
                    entry["decoded"] = {
                        field_name: (numbers[field["word"]] >> field["lsb"]) & ((1 << field["width"]) - 1)
                        for field_name, field in spec["fields"].items()
                    }
            if tag == "read-failed":
                failures.append(row)
            reads[name] = entry
        else:
            failures.append(row)
    require(set(cores) == set(profile["cores"]), "OpenOCD response is missing core states")
    return {"soc": profile["soc"], "revision": profile["revision"], "clock": clock,
            "start_ns": start, "end_ns": end, "cores": cores, "reads": reads,
            "halt_group": group, "errors": failures}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    capture = commands.add_parser("capture")
    capture.add_argument("--halt-group", help="temporarily halt this declared group")
    capture.add_argument("--clock", help="operator-calibrated capture clock ID")
    attach = commands.add_parser("attach")
    attach.add_argument("core")
    attach.add_argument("--elf", type=Path)
    attach.add_argument("--gdb", default="gdb")
    args = parser.parse_args()
    try:
        profile = load_object(args.profile)
        validate(profile)
        if args.command == "attach":
            core = profile["cores"][args.core]
            command = [args.gdb, "-nx"]
            if args.elf:
                command += ["--se", str(args.elf.resolve(strict=True))]
            command += ["-ex", f"target remote 127.0.0.1:{core['gdb_port']}"]
            return subprocess.call(command)
        group = profile["halt_groups"][args.halt_group] if args.halt_group else []
        report = snapshot(profile, group, args.clock)
        print(json.dumps(report, indent=2))
        return int(bool(report["errors"]) or any(c["state"] == "unavailable"
                                                  for c in report["cores"].values()))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(2, f"SoC debug: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
