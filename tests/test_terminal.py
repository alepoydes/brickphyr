# SPDX-License-Identifier: Apache-2.0
"""Real CLI and TTY flow control against a deliberately stalled serial PTY."""

import fcntl
import os
import select
import subprocess
import termios
import time

import pytest


@pytest.mark.parametrize("resume", [False, True])
def test_terminal_backpressure_and_exit(resume):
    keyboard, tty = os.openpty()
    peer, port = os.openpty()
    original = termios.tcgetattr(tty)
    process = subprocess.Popen(
        ["uv", "run", "--no-sync", "brickphyrdev", "terminal", "--port", os.ttyname(port)],
        stdin=tty,
        stdout=tty,
        stderr=tty,
        start_new_session=True,
        preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY),
    )
    output = bytearray()

    def read_output():
        while select.select([keyboard], [], [], 0)[0]:
            output.extend(os.read(keyboard, 8192))

    def alive():
        read_output()
        assert process.poll() is None, output.decode(errors="replace")

    try:
        deadline = time.monotonic() + 3
        while termios.tcgetattr(port)[4] != termios.B115200:
            alive()
            assert time.monotonic() < deadline, "Console did not open the serial PTY"
            time.sleep(0.005)
        # A plain terminal must preserve arbitrary output, including bytes that
        # would be a framing prefix in an explicitly managed SMP connection.
        binary = b"plain output\x06\x09not an SMP frame\x00\r\n"
        os.write(peer, binary)
        deadline = time.monotonic() + 1
        while binary not in output:
            alive()
            assert time.monotonic() < deadline, output
            time.sleep(0.005)
        os.set_blocking(keyboard, False)
        # Stop draining the simulated wire. A large paste then fills the serial
        # driver, the CLI's bounded queue and finally the local TTY input buffer.
        payload = b"abc\x03\x1a\x1c\x00\x7f\x1b[A\r" * 8192
        sent = 0
        deadline = time.monotonic() + (0.2 if resume else 2)
        while time.monotonic() < deadline:
            alive()
            if select.select([], [keyboard], [], 0.005)[1] and sent < len(payload):
                try:
                    end = len(payload) if resume else sent + 256
                    sent += os.write(keyboard, payload[sent:end])
                except BlockingIOError:
                    pass
            if not resume:
                # Stop once input remains unread by the CLI. Leave TTY space
                # for a subsequent key, as with interactive typing, rather
                # than blocking the terminal emulator itself on a huge paste.
                time.sleep(0.005)
                if sent > 4096 and select.select([tty], [], [], 0)[0]:
                    break
        alive()
        assert 0 < sent < len(payload), "The synthetic wire did not exert backpressure"

        if resume:
            received = bytearray()
            deadline = time.monotonic() + 3
            while len(received) < len(payload):
                alive()
                assert time.monotonic() < deadline, "Input did not resume after the wire drained"
                readers, writers, _ = select.select(
                    [peer], [keyboard] if sent < len(payload) else [], [], 0.01
                )
                if readers:
                    received.extend(os.read(peer, 8192))
                if writers:
                    try:
                        sent += os.write(keyboard, payload[sent:])
                    except BlockingIOError:
                        pass
            assert received == payload

        # This must also work with the sender blocked and the input queue full.
        os.write(keyboard, b"\x1d")
        assert process.wait(timeout=1) == 0
        read_output()
        assert b"Terminal input failed" not in output
        assert termios.tcgetattr(tty) == original
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=2)
        for fd in (keyboard, tty, peer, port):
            os.close(fd)
