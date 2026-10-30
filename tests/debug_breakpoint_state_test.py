"""Managed records survive failed clears and are released on CLI/TUI exit."""

import fcntl
import os
from pathlib import Path
import pty
import re
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

from debug_run_control_test import Target, packet


def record(direction, data=b"", status=None):
    if status is None:
        status = len(data)
    return struct.pack("<c3xiQI", direction, status, 0, len(data)) + data


def transaction(command, reply):
    request = record(b"W", packet(command.encode()))
    if reply is None:
        return request + record(b"R", status=-3)  # Transport closed before ACK.
    return request + record(b"R", b"+" + packet(reply)) + record(b"W", b"+")


def test_cli(binary, watchpoint=False, disconnected=False):
    addresses = (0x20000000, 0x20000004) if watchpoint else (0x08001000, 0x08002000)
    types = (2,) if watchpoint else (1,)  # Managed clears use the recorded type.
    add = "watch" if watchpoint else "break"
    delete = "delete watch" if watchpoint else "clear"
    noun = "watchpoint" if watchpoint else "breakpoint"
    commands = [f"{add} 0x{addr:08x}" for addr in addresses]
    commands += [f"{delete} 999", f"{delete} 1", f"info {noun}s",
                 f"{delete} 1", f"info {noun}s", f"{delete} 2",
                 f"info {noun}s", f"{delete} 2", "quit"]
    capture = b"MKDBGCAP" + struct.pack("<I32sII", 1, b"cortex-m", 0, 17)
    for addr in addresses:
        capture += transaction(f"Z{types[0]},{addr:x},{1 if watchpoint else 4}", b"OK")
    for kind in types:
        capture += transaction(f"z{kind},{addresses[0]:x},{1 if watchpoint else 4}",
                               None if disconnected else b"E01")
    for addr in addresses:
        capture += transaction(f"z{types[0]},{addr:x},{1 if watchpoint else 4}", b"OK")
    capture += record(b"W", packet(b"c"))
    with tempfile.TemporaryDirectory(prefix="mkdbg-clear-state-") as directory:
        path = Path(directory) / "clear.cap"
        path.write_bytes(capture)
        result = subprocess.run(
            [binary, "debug", "--replay-debug", str(path)],
            input="\n".join(commands) + "\n", text=True, capture_output=True, timeout=5,
        )
    assert result.returncode == 0, (result.stdout, result.stderr)
    rows = re.findall(r"^(\d+)\s+(0x[0-9a-f]+)(?:\s+\w+)?$", result.stdout, re.MULTILINE)
    expected = [("1", f"0x{addresses[0]:08x}"), ("2", f"0x{addresses[1]:08x}")]
    assert rows == expected + expected[1:], result.stdout
    assert "record retained" in result.stdout, result.stdout
    assert result.stdout.count(f"No {noun}s.") == 1, result.stdout
    assert f"error: no {noun} 999" in result.stdout, result.stdout
    assert f"error: no {noun} 2" in result.stdout, result.stdout
    assert f"{noun.capitalize()} 1 (0x{addresses[0]:08x}) cleared." in result.stdout, result.stdout
    assert f"{noun.capitalize()} 2 (0x{addresses[1]:08x}) cleared." in result.stdout, result.stdout


def test_exit_cleanup(binary, eof=False, failed_clear=None):
    master, slave = pty.openpty()
    process = None
    try:
        process = subprocess.Popen(
            [binary, "debug", "--port", os.ttyname(slave)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        commands = b"break 0x08001000\nrwatch 0x20000004\n"
        process.stdin.write(commands + (b"" if eof else b"quit\n"))
        process.stdin.close()
        process.stdin = None
        target = Target(master)
        for request in (b"Z1,8001000,4", b"Z3,20000004,1"):
            assert target.event() == request
            os.write(master, b"+" + packet(b"OK"))
        for request, kind in ((b"z1,8001000,4", "breakpoint"),
                              (b"z3,20000004,1", "watchpoint")):
            assert target.event() == request, "exit resumed before clearing owned records"
            os.write(master, b"+" + packet(b"E01" if failed_clear == kind else b"OK"))
            if failed_clear == kind:
                break
        if failed_clear is None:
            assert target.event() == b"c", "exit must resume exactly once after cleanup"
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == (1 if failed_clear else 0), (stdout, stderr)
        if failed_clear:
            assert b"detach failed" in stderr and b"target resume not confirmed" in stderr, stderr
        else:
            assert b"detach failed" not in stderr, stderr
        try:
            extra = target.event(timeout=0.05)
        except AssertionError as error:
            assert str(error) == "timed out waiting for debugger request", error
        else:
            raise AssertionError(("unexpected request after detach", extra))
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.communicate()
        os.close(master)
        os.close(slave)


def test_resume_write_failure(binary):
    capture = b"MKDBGCAP" + struct.pack("<I32sII", 1, b"cortex-m", 0, 17)
    capture += record(b"W", packet(b"c"), status=-1)
    with tempfile.TemporaryDirectory(prefix="mkdbg-detach-failure-") as directory:
        path = Path(directory) / "resume-failed.cap"
        path.write_bytes(capture)
        for commands in ("quit\n", ""):
            result = subprocess.run(
                [binary, "debug", "--replay-debug", str(path)],
                input=commands, text=True, capture_output=True, timeout=5,
            )
            assert result.returncode == 1, (result.stdout, result.stderr)
            assert "detach failed" in result.stderr, result.stderr
        # A directory fd makes fgets fail with an input error rather than EOF.
        input_fd = os.open(directory, os.O_RDONLY)
        try:
            result = subprocess.run(
                [binary, "debug", "--replay-debug", str(path)],
                stdin=input_fd, text=True, capture_output=True, timeout=5,
            )
        finally:
            os.close(input_fd)
        assert result.returncode == 1, (result.stdout, result.stderr)
        assert "failed to read debug command" in result.stderr, result.stderr
        assert "detach failed" in result.stderr, "input failure skipped the detach attempt"


def test_tui(binary, quit_in_tui=False):
    master, slave = pty.openpty()
    port = os.ttyname(slave)
    pid, terminal = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
        os.environ["TERM"] = "xterm-256color"
        os.execv(binary, [binary, "debug", "--port", port])

    output = bytearray()

    def drain_terminal():
        try:
            while True:
                data = os.read(terminal, 65536)
                if not data:
                    return
                output.extend(data)
        except OSError:
            pass  # PTY EOF after the child exits.

    reader = threading.Thread(target=drain_terminal, daemon=True)
    reader.start()
    target = Target(master)

    def serve_until(wanted):
        for _ in range(30):
            try:
                event = target.event()
            except AssertionError as error:
                raise AssertionError((wanted, bytes(output[-2000:]))) from error
            if event == wanted:
                return
            if event == b"g":
                os.write(master, b"+" + packet(b"00000000" * 17))
            elif event.startswith(b"m"):
                length = int(event.split(b",")[1], 16)
                os.write(master, b"+" + packet(b"00" * length))
            else:
                raise AssertionError((wanted, event, bytes(output[-2000:])))
        raise AssertionError(("too many requests", wanted))

    def return_to_cli(count):
        deadline = time.monotonic() + 5
        while output.count(b"back in CLI mode") < count:
            if time.monotonic() >= deadline:
                raise AssertionError(bytes(output[-2000:]))
            try:
                event = target.event(timeout=0.1)
            except AssertionError:
                continue
            assert event.startswith(b"m"), event
            length = int(event.split(b",")[1], 16)
            os.write(master, b"+" + packet(b"00" * length))

    try:
        os.write(terminal, b"tui\n")
        serve_until(b"g")  # TUI is active; subsequent keys bypass CLI buffering.
        os.write(master, b"+" + packet(b"00000000" * 17))
        os.write(terminal, b"b0x100000000\rw0x100000000\rb0x08001000junk\r"
                          b"b0x08001000\rb0x08002000\rd1\rd1\rd2\rd2\rb0x08003000\rt")
        requests = [(b"Z1,8001000,4", b"OK"), (b"Z1,8002000,4", b"OK"),
                    (b"z1,8001000,4", b"E01"), (b"z1,8001000,4", b"OK"),
                    (b"z1,8002000,4", b"OK"), (b"Z1,8003000,4", b"OK")]
        for request, reply in requests:
            serve_until(request)
            os.write(master, b"+" + packet(reply))
        return_to_cli(1)
        os.write(terminal, b"info breakpoints\nclear 3\nbreak 0x08004000\nrwatch 0x20000000\ntui\n")
        for request in (b"z1,8003000,4", b"Z1,8004000,4", b"Z3,20000000,1"):
            serve_until(request)
            os.write(master, b"+" + packet(b"OK"))
        serve_until(b"g")
        os.write(master, b"+" + packet(b"00000000" * 17))
        os.write(terminal, b"d4\rw0x20000004\r" + (b"q" if quit_in_tui else b"t"))
        for request in (b"z1,8004000,4", b"Z2,20000004,1"):
            serve_until(request)
            os.write(master, b"+" + packet(b"OK"))
        if quit_in_tui:
            clears = (b"z2,20000004,1", b"z3,20000000,1")
        else:
            return_to_cli(2)
            os.write(terminal, b"info watchpoints\ndelete watch 1\ndelete watch 2\ninfo watchpoints\nquit\n")
            clears = (b"z3,20000000,1", b"z2,20000004,1")
        for request in clears:
            serve_until(request)
            os.write(master, b"+" + packet(b"OK"))
        serve_until(b"c")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            got, status = os.waitpid(pid, os.WNOHANG)
            if got:
                pid = None
                assert os.waitstatus_to_exitcode(status) == 0, bytes(output[-2000:])
                break
            time.sleep(0.01)
        assert pid is None, "TUI did not exit"
        reader.join(timeout=1)
        assert re.search(rb"3\s+0x08003000", output), bytes(output[-2000:])
        if not quit_in_tui:
            assert re.search(rb"1\s+0x20000000\s+read", output), bytes(output[-2000:])
            assert re.search(rb"2\s+0x20000004\s+write", output), bytes(output[-2000:])
            assert b"No watchpoints." in output, bytes(output[-2000:])
    finally:
        if pid is not None:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        reader.join(timeout=1)
        os.close(terminal)
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    binary = os.path.abspath(sys.argv[1])
    test_cli(binary)
    test_cli(binary, disconnected=True)
    test_cli(binary, watchpoint=True)
    for eof in (False, True):
        for failed_clear in (None, "breakpoint", "watchpoint"):
            test_exit_cleanup(binary, eof=eof, failed_clear=failed_clear)
    test_resume_write_failure(binary)
    test_tui(binary)
    test_tui(binary, quit_in_tui=True)
    print("debug_breakpoint_state_test: OK")
