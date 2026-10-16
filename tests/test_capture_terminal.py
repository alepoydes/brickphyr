# SPDX-License-Identifier: Apache-2.0
"""Interactive Shell and finite capture collection over one ARM console."""

import asyncio
import fcntl
import os
import selectors
import subprocess
import sys
import termios
import time
from pathlib import Path

import pytest
from test_native import native_console

from brickphyrdev import command_file, console, recording
from brickphyrdev.records import Recording
from brickphyrdev.smp import checked

BUILD = Path(__file__).resolve().parents[1] / "build/native-smp-tests"


def test_file_failure_preserves_interactive_input_and_does_not_start(tmp_path):
    path = tmp_path / "rejected.txt"
    path.write_text("capture prepare 20 300 battery\n"
                    "capture at 50 100 A=invalid\n"
                    "capture start\n", encoding="utf-8")

    async def exercise(port):
        output = bytearray()
        async with console.connection(output.extend, port=port, management=True) as (link, client):
            await link.write(b"config get power/usb_idle_")
            async with asyncio.timeout(2):
                while b"usb_idle_" not in output:
                    await asyncio.sleep(0)
            with pytest.raises(command_file.CommandError, match=r"rejected\.txt:2:.*ret=-22"):
                await command_file.execute(client, command_file.load_commands(path), lambda _: None)
            assert (await checked(client, recording.Info())).size == 0
            reply = await checked(client, command_file.Execute(argv=["help"]))
            assert reply.ret == 0 and reply.truncated and reply.o
            await link.write(b"s\r")
            async with asyncio.timeout(2):
                while b"0 s (selected and effective)" not in output:
                    await asyncio.sleep(0)

    with native_console(BUILD) as port:
        asyncio.run(asyncio.wait_for(exercise(port), 5))


@pytest.mark.parametrize("series", [False, True])
def test_terminal_collects_capture_without_losing_shell(tmp_path, series):
    with native_console(BUILD) as port:
        master, slave = os.openpty()
        original = termios.tcgetattr(slave)
        record = tmp_path / "capture.cbor"
        directory = tmp_path / "captures"
        commands = tmp_path / "experiment.txt"
        commands.write_text("# Same commands as interactive Shell\n"
                            "capture prepare 20 300 battery\n"
                            "capture check\n"
                            "capture start\n", encoding="utf-8")
        process = subprocess.Popen(
            [
                Path(sys.executable).with_name("brickphyrdev"),
                "terminal", "--port", port,
                *(["--record-dir", str(directory)] if series else ["--record", str(record)]),
                "--commands", str(commands),
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

                until(b"Command file complete; keyboard input enabled.")
                assert b"Capture: prepared" in output
                command(b"config get power/usb_idle_s", b"0 s (selected and effective)")

                for attempt in range(2 if series else 1):
                    if attempt:
                        command(b"capture start", b"brickphyr> ")
                    view_start = len(output)
                    os.write(master, b"\x14")
                    deadline = time.monotonic() + 5
                    last_hide = view_start

                    def download_visible():
                        return (b"Download complete; hub data retained" in output[view_start:] and
                                (not series or f"Completed downloads: {attempt + 1}".encode()
                                 in output[view_start:]))

                    while not download_visible():
                        assert process.poll() is None, output.decode(errors="replace")
                        remaining = deadline - time.monotonic()
                        assert remaining > 0 and selector.select(remaining), output.decode(
                            errors="replace"
                        )
                        output.extend(os.read(master, 8192))
                        hidden = output.rfind(b"\x1b[?1049l", view_start)
                        if hidden >= last_hide and not download_visible():
                            # Async Shell output may temporarily restore its real screen.
                            last_hide = hidden + 1
                            os.write(master, b"\x14")
                    assert b"0 gaps" in output[view_start:]
                    os.write(master, b"\x14")
                    command(b"capture status", b"Capture: retained")
                    command(b"capture clear", b"brickphyr> ")
                    command(b"capture status", b"Capture: prepared")
                os.write(master, b"\x1d")
                assert process.wait(timeout=3) == 0, output.decode(errors="replace")

            assert termios.tcgetattr(slave) == original
            paths = list(directory.glob("*.cbor")) if series else [record]
            assert len(paths) == (2 if series else 1)
            for path in paths:
                with path.open("rb") as source:
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
