"""Malformed interactive numbers must not produce target requests."""

import os
import pty
import subprocess
import sys

from debug_run_control_test import Target, packet


def test_invalid_numbers(binary):
    invalid = []
    for command in ("break", "watch", "rwatch", "awatch", "mem", "display"):
        for address in ("0x100000000", "0x08001000junk", "-1", "+1"):
            invalid.append(f"{command} {address}")
    for command in ("clear", "delete watch", "undisplay"):
        for identifier in ("1junk", "-1", "+1", "0", "2147483648", "1 2"):
            invalid.append(f"{command} {identifier}")
    for length in ("1junk", "0", "257", "-1", "+1", "1 2"):
        invalid.append(f"mem 0x20000000 {length}")
    invalid.append("break 0x" + "0" * 300 + "1000")
    master, slave = pty.openpty()
    process = None
    try:
        process = subprocess.Popen(
            [binary, "debug", "--port", os.ttyname(slave)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        setup = ["break 0x08001000", "rwatch 0x20000004", "display 0x20000010"]
        finish = ["mem 0x20000000 1", "clear 1", "delete watch 1", "undisplay 1", "quit"]
        process.stdin.write(("\n".join(setup + invalid + finish) + "\n").encode())
        process.stdin.flush()
        target = Target(master)
        for request in (b"Z1,8001000,4", b"Z3,20000004,1"):
            assert target.event() == request
            os.write(master, b"+" + packet(b"OK"))
        request = target.event()
        assert request == b"m20000000,1", ("invalid input reached target", request)
        os.write(master, b"+" + packet(b"aa"))
        for request in (b"z1,8001000,4", b"z3,20000004,1"):
            assert target.event() == request, "invalid ID removed the valid record"
            os.write(master, b"+" + packet(b"OK"))
        assert target.event() == b"c"
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, (stdout, stderr)
        assert stdout.count(b"error:") == len(invalid), stdout
        assert b"0x20000000: aa" in stdout, stdout
        assert b"display 1 removed" in stdout, stdout
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.communicate()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    test_invalid_numbers(sys.argv[1])
    print("debug_numeric_input_test: OK")
