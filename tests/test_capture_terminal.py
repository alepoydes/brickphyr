# SPDX-License-Identifier: Apache-2.0
"""Interactive Shell and finite capture collection over one ARM console."""

import fcntl
import os
import selectors
import subprocess
import sys
import termios
import time
from pathlib import Path

from test_native import native_console

from brickphyrdev.records import Recording

BUILD = Path(__file__).resolve().parents[1] / "build/native-smp-tests"


def test_terminal_collects_capture_without_losing_shell(tmp_path):
    with native_console(BUILD) as port:
        master, slave = os.openpty()
        original = termios.tcgetattr(slave)
        record = tmp_path / "capture.cbor"
        process = subprocess.Popen(
            [
                Path(sys.executable).with_name("brickphyrdev"),
                "terminal", "--port", port, "--record", str(record),
            ],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            start_new_session=True,
            preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY),
        )
        output = bytearray()
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(master, selectors.EVENT_READ)

                def until(marker, start=0, timeout=4):
                    deadline = time.monotonic() + timeout
                    while marker not in output[start:]:
                        assert process.poll() is None, output.decode(errors="replace")
                        remaining = deadline - time.monotonic()
                        assert remaining > 0 and selector.select(remaining), output.decode(
                            errors="replace"
                        )
                        output.extend(os.read(master, 8192))

                def command(line, marker):
                    start = len(output)
                    os.write(master, line + b"\r")
                    until(marker, start)

                until(b"Ctrl+T switches the status view")
                command(b"capture prepare 20 300 battery", b"Capture: prepared")
                command(b"capture start", b"brickphyr> ")
                command(b"capture status", b"Capture: running")
                command(b"config get power/usb_idle_s", b"0 s (selected and effective)")

                view_start = len(output)
                os.write(master, b"\x14")
                deadline = time.monotonic() + 5
                last_hide = view_start
                while b"Download complete; hub data retained" not in output[view_start:]:
                    assert process.poll() is None, output.decode(errors="replace")
                    remaining = deadline - time.monotonic()
                    assert remaining > 0 and selector.select(remaining), output.decode(
                        errors="replace"
                    )
                    output.extend(os.read(master, 8192))
                    hidden = output.rfind(b"\x1b[?1049l", view_start)
                    if (hidden >= last_hide and
                        b"Download complete; hub data retained" not in output[view_start:]):
                        # Async Shell output may temporarily restore its real screen.
                        last_hide = hidden + 1
                        os.write(master, b"\x14")
                assert b"0 gaps" in output
                os.write(master, b"\x14")
                command(b"capture status", b"Capture: retained")
                command(b"capture clear", b"brickphyr> ")
                command(b"capture status", b"Capture: prepared")
                os.write(master, b"\x1d")
                assert process.wait(timeout=3) == 0, output.decode(errors="replace")

            assert termios.tcgetattr(slave) == original
            with record.open("rb") as source:
                captured = Recording(source)
                frames = list(captured.frames())
            assert frames and captured.complete and captured.gaps == 0
            assert captured.outcome["reason"] == "duration"
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)
            os.close(master)
            os.close(slave)
