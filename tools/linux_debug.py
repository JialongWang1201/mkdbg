#!/usr/bin/env python3
"""Inspect a Linux kdump image or attach to a live KGDB target."""

import argparse
import ipaddress
import re
import subprocess
import tempfile
from pathlib import Path

from board_evidence import require

SECTIONS = {
    "system": "sys",
    "panic": "log",
    "cpus": "bt -a",
    "threads": "ps",
    "address-spaces": "vm",
    "modules": "mod",
    "interrupts": "irq",
}


def readable_file(path):
    result = path.resolve(strict=True)
    require(result.is_file(), f"{result}: expected a regular file")
    return str(result)


def kgdb_endpoint(endpoint):
    if endpoint.startswith("/dev/"):
        require(re.fullmatch(r"/dev/[A-Za-z0-9_./-]+", endpoint),
                "invalid KGDB serial device")
        require(Path(endpoint).is_char_device(), f"{endpoint}: serial device unavailable")
        return endpoint
    address, separator, number = endpoint.rpartition(":")
    require(separator and address and re.fullmatch(r"[0-9]{1,5}", number),
            "KGDB endpoint must be a serial device or host:port")
    require(1 <= int(number) <= 65535, "invalid KGDB TCP port")
    if address.startswith("[") and address.endswith("]"):
        ipaddress.IPv6Address(address[1:-1])
    else:
        require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]*", address), "invalid KGDB TCP host")
    return endpoint


def kgdb_command(args):
    image = readable_file(args.vmlinux)
    endpoint = kgdb_endpoint(args.endpoint)
    result = [args.gdb, "-q", "-nx", "-iex", "set auto-load off", "--se", image]
    if args.batch:
        result += ["-batch"]
    if args.baud is not None:
        require(endpoint.startswith("/dev/") and 1 <= args.baud <= 4000000,
                "--baud requires a serial endpoint and a supported rate")
        result += ["-ex", f"set serial baud {args.baud}"]
    result += ["-ex", f"target remote {endpoint}"]
    if args.batch:
        result += ["-ex", "info threads", "-ex", "thread apply all bt"]
    return result


def dump_commands(sections, pid, modules):
    commands = []
    for spec in modules:
        name, separator, path = spec.partition("=")
        require(separator and re.fullmatch(r"[a-zA-Z0-9_-]+", name),
                "--module requires NAME=PATH")
        file_name = readable_file(Path(path))
        require(re.fullmatch(r"/[A-Za-z0-9_./+-]+", file_name),
                f"module path contains characters unsupported by the crash command parser: {file_name}")
        commands.append(f"mod -s {name} {file_name}")
    for section in sections:
        command = SECTIONS[section]
        if section == "address-spaces" and pid is not None:
            command += f" {pid}"
        commands.append(command)
    if pid is not None:
        commands.append(f"bt {pid}")
    commands.append("exit")
    return commands


def inspect_dump(args):
    vmlinux = readable_file(args.vmlinux)
    vmcore = readable_file(args.vmcore)
    if args.pid is not None:
        require(0 <= args.pid <= 2 ** 31 - 1, "invalid Linux PID")
    sections = args.section or SECTIONS
    commands = dump_commands(sections, args.pid, args.module)
    with tempfile.TemporaryDirectory(prefix="mkdbg-kdump-") as directory:
        script = Path(directory) / "commands"
        script.write_text("\n".join(commands) + "\n", encoding="ascii")
        return subprocess.call([args.crash, "-s", "-i", str(script), vmlinux, vmcore])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(required=True, dest="mode")

    live = modes.add_parser("kgdb", help="attach GDB to a configured Linux debug endpoint")
    live.add_argument("--vmlinux", type=Path, required=True)
    live.add_argument("--endpoint", required=True, metavar="DEVICE_OR_HOST:PORT")
    live.add_argument("--gdb", default="gdb")
    live.add_argument("--baud", type=int)
    live.add_argument("--batch", action="store_true", help="print threads and stacks, then exit")

    dump = modes.add_parser("kdump", help="inspect a saved vmcore with crash")
    dump.add_argument("--vmlinux", type=Path, required=True)
    dump.add_argument("--vmcore", type=Path, required=True)
    dump.add_argument("--crash", default="crash")
    dump.add_argument("--section", action="append", choices=SECTIONS)
    dump.add_argument("--pid", type=int, help="inspect this task's mappings and backtrace")
    dump.add_argument("--module", action="append", default=[], metavar="NAME=PATH")

    args = parser.parse_args()
    try:
        if args.mode == "kgdb":
            return subprocess.call(kgdb_command(args))
        return inspect_dump(args)
    except (OSError, ValueError) as exc:
        parser.exit(2, f"Linux debug: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
