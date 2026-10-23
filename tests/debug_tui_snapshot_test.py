"""Failed register reads/control operations must not reuse stale PC context."""

import fcntl
import os
import pty
import signal
import struct
import sys
import termios
import threading
import time

from debug_run_control_test import Target, packet


def registers(pc):
    result = bytearray(b"00000000" * 17)
    result[15 * 8:16 * 8] = pc.to_bytes(4, "little").hex().encode()
    return result


def test_snapshot(binary):
    master, slave = pty.openpty()
    port = os.ttyname(slave)
    pid, terminal = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 100, 0, 0))
        os.environ["TERM"] = "xterm-256color"
        os.execv(binary, [binary, "debug", "--port", port])

    output = bytearray()

    def drain():
        try:
            while True:
                data = os.read(terminal, 65536)
                if not data:
                    return
                output.extend(data)
        except OSError:
            pass

    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    target = Target(master)

    def reply(payload):
        os.write(master, b"+" + packet(payload))

    def expect_step(pc):
        saw_memory = False
        for _ in range(10):
            request = target.event()
            if request == b"s":
                assert saw_memory, "valid snapshot did not restore assembly context"
                return
            assert request.startswith(b"m"), request
            addr, length = (int(value, 16) for value in request[1:].split(b","))
            assert addr == pc - 64, ("stale PC used for assembly", request)
            reply(b"00" * length)
            saw_memory = True
        raise AssertionError("too many memory requests")

    try:
        os.write(terminal, b"tui\n")
        assert target.event() == b"g"
        reply(registers(0x08001234))
        os.write(terminal, b"s")
        expect_step(0x08001234)
        reply(b"S05")
        assert target.event() == b"g"
        reply(b"E01")

        # A failed read must hide the old PC and avoid all PC-derived reads.
        os.write(terminal, b"s")
        assert target.event() == b"s", "failed register read retained stale PC context"
        reply(b"S05")
        assert target.event() == b"g"
        reply(registers(0x08002000))

        # A subsequent successful read restores context using the new PC.
        os.write(terminal, b"s")
        expect_step(0x08002000)
        reply(b"E01")
        os.write(terminal, b"q")
        assert target.event() == b"c", "failed execution control retained stale PC context"
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            got, status = os.waitpid(pid, os.WNOHANG)
            if got:
                pid = None
                assert os.waitstatus_to_exitcode(status) == 0
                break
            time.sleep(0.01)
        assert pid is None, "TUI did not exit"
        reader.join(timeout=1)
        assert b"Registers unavailable" in output, bytes(output[-2000:])
    finally:
        if pid is not None:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        reader.join(timeout=1)
        os.close(terminal)
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    test_snapshot(os.path.abspath(sys.argv[1]))
    print("debug_tui_snapshot_test: OK")
