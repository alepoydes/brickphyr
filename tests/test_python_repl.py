# SPDX-License-Identifier: Apache-2.0
"""Maintained MicroPython REPL through the protected application and SMP path."""

import asyncio
import fcntl
import os
import select
import subprocess
import sys
import termios
import time
from pathlib import Path

import pytest
from test_native import native_console
from test_python import completed, output
from test_python_io import BUILD, until

from brickphyrdev import console, repl
from brickphyrdev.application import (
    Format,
    Input,
    Interrupt,
    Repl,
    Start,
    Status,
    Stop,
    checked,
    connection,
    upload,
)


def test_repl_editing_errors_and_interrupt():
    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                await checked(client, Repl())
                assert b"MicroPython" in await until(client, b">>> ")
                # Readline handles deletion, cursor movement, history and completion.
                for text, expected in (
                    (b"answer = 40\r", b">>> "),
                    (b"answer + 3\x7f2\r", b"\r\n42\r\n>>> "),
                    (b"\x1b[A\r", b"\r\n42\r\n>>> "),
                    (b"answer + 3\x1b[D\x1b[3~2\r", b"\r\n42\r\n>>> "),
                    (b"answ\t + 2\r", b"\r\n42\r\n>>> "),
                    (b"1 / 0\r", b"ZeroDivisionError"),
                ):
                    await checked(client, Input(data=text))
                    result = await until(client, b">>> ")
                    assert expected in result, result
                await checked(client, Input(data=b"incomplete = "))
                await checked(client, Interrupt())
                await until(client, b">>> ")
                await checked(client, Input(data=b"while True: pass\r\r"))
                await asyncio.sleep(0.03)
                await checked(client, Interrupt())
                result = await until(client, b">>> ")
                assert b"KeyboardInterrupt" in result, result
                await checked(client, Input(data=b"name = input('Name: ')\r"))
                await until(client, b"Name: ")
                await checked(client, Input(data=b"Brickphyr\r"))
                await until(client, b">>> ")
                await checked(client, Input(data=b"print('Hello', name)\r"))
                assert b"Hello Brickphyr\r\n" in await until(client, b">>> ")
                state = await checked(client, Status())
                assert state.repl and state.state == 1 and not state.dropped
            finally:
                await checked(client, Stop())
            await upload(client, transport, b"print('after REPL')\n", Format.PYTHON)
            await checked(client, Start())
            assert (await completed(client)).result == 0
            assert b"after REPL" in await output(client)

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_repl_soft_reset_native_code_and_raw_protocol():
    async def exercise(port):
        async with connection(port=port) as (client, _):
            try:
                await checked(client, Repl())
                await until(client, b">>> ")
                await checked(
                    client,
                    Input(
                        data=(
                            b"\x05import micropython\n@micropython.native\n"
                            b"def add(x): return x + 2\n\x04"
                        )
                    ),
                )
                await until(client, b">>> ")
                await checked(client, Input(data=b"add(40)\r"))
                assert b"\r\n42\r\n>>> " in await until(client, b">>> ")
                assert (await checked(client, Status())).code_used > 0
                await checked(client, Input(data=b"\x04"))
                assert b"MPY: soft reboot" in await until(client, b">>> ")
                state = await checked(client, Status())
                assert state.repl and state.state == 1 and state.code == state.code_used == 0
                await checked(client, Input(data=b"add(40)\r"))
                assert b"NameError" in await until(client, b">>> ")
                await checked(client, Input(data=b"\x01"))
                await until(client, b"raw REPL; CTRL-B to exit\r\n>")
                await checked(client, Input(data=b"print(6 * 7)\x04"))
                assert b"OK42\r\n\x04\x04>" in await until(client, b"\x04\x04>")
                await checked(client, Input(data=b"\x04"))
                assert b"MPY: soft reboot" in await until(client, b"raw REPL; CTRL-B to exit\r\n>")
                await checked(client, Input(data=b"\x02"))
                await until(client, b">>> ")
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_repl_admission_and_reattach():
    async def exercise(port):
        async with connection(port=port) as (client, transport):
            await upload(client, transport, b"while True: pass\n", Format.PYTHON)
            await checked(client, Start())
            with pytest.raises(RuntimeError, match="error 16"):
                await checked(client, Repl())
            assert not (await checked(client, Status())).repl
            await checked(client, Stop())
            await checked(client, Repl())
            await until(client, b">>> ")
            await checked(client, Input(data=b"answer = 42\r"))
            await until(client, b">>> ")
        # Plain QEMU UART does not notify firmware of a transport disconnect.
        # This covers admission and reattachment, not physical partial-line cleanup.
        async with connection(port=port) as (client, _):
            try:
                await checked(client, Repl())
                await checked(client, Input(data=b"answer\r"))
                assert b"\r\n42\r\n>>> " in await until(client, b">>> ")
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_repl_watcher_drains_stopped_output():
    async def exercise(port):
        async with connection(port=port) as (client, _):
            try:
                await checked(client, Repl())
                await until(client, b">>> ")
                await checked(client, Input(data=b"print('0123456789' * 80 + '!')\r"))
                async with asyncio.timeout(3):
                    while (await checked(client, Status())).queued < 830:
                        await asyncio.sleep(0.01)
                await checked(client, Stop())
                result = bytearray()
                await asyncio.wait_for(repl.Session(client, result.extend).watch(), 3)
                assert b"0123456789" * 80 + b"!\r\n" in result
                assert b"[REPL ended: result 0, fault 0, cleanup 0]" in result
                assert (await checked(client, Status())).queued == 0
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_repl_partial_input_at_session_boundary():
    async def exercise(port):
        shell_output = bytearray()
        boundary = asyncio.Event()

        def display(data):
            shell_output.extend(data)
            if b"Synthetic session boundary" in shell_output:
                boundary.set()

        async with console.connection(display, management=True, port=port) as (transport, client):
            try:
                await checked(client, Repl())
                await until(client, b">>> ")
                await checked(client, Input(data=b"answer = 42\r"))
                await until(client, b">>> ")
                await checked(client, Input(data=b"discarded = 91"))
                await until(client, b"discarded = 91")
                # Only the detach notification is synthetic. No VM or I/O handler
                # is replaced, and the subsequent input uses the same SMP path.
                await transport.write(b"session_close\r")
                await asyncio.wait_for(boundary.wait(), 2)
                await until(client, b">>> ")
                await checked(client, Input(data=b"print(answer, 'discarded' in globals())\r"))
                assert b"\r\n42 False\r\n" in await until(client, b">>> ")
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


@pytest.mark.parametrize("detach", [False, True])
def test_repl_terminal_cli(detach, tmp_path):
    with native_console(BUILD) as port:
        keyboard, tty = os.openpty()
        original = termios.tcgetattr(tty)
        monitoring = ["--monitor", "--source", "application", "--log", str(tmp_path / "repl.jsonl")]
        process = subprocess.Popen(
            [
                Path(sys.executable).with_name("brickphyrdev"),
                "terminal",
                "--port",
                port,
                "--repl",
                *(monitoring if detach else []),
            ],
            stdin=tty,
            stdout=tty,
            stderr=tty,
            start_new_session=True,
            preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY),
        )

        def received(marker):
            data = bytearray()
            deadline = time.monotonic() + 4
            while marker not in data:
                assert process.poll() is None, data
                assert time.monotonic() < deadline, (marker, data)
                if select.select([keyboard], [], [], 0.05)[0]:
                    data.extend(os.read(keyboard, 8192))
            return data

        try:
            received(b">>> ")
            os.write(keyboard, b"answer = 42\r")
            received(b">>> ")
            os.write(keyboard, b"answer\r")
            assert b"\r\n42\r\n" in received(b">>> ")
            os.write(keyboard, b"import sys; sys.stdout.buffer.write(bytes([6, 9, 0, 4]))\r")
            assert b"\x06\x09\x00\x04" in received(b">>> ")
            # Both initial attachment and a soft reset retain lossless output.
            for reset in (False, True):
                if reset:
                    os.write(keyboard, b"\x04")
                    received(b">>> ")
                    os.write(keyboard, b"answer = 42\r")
                    received(b">>> ")
                os.write(keyboard, b"print('0123456789' * 409 + '!')\r")
                result = received(b">>> ")
                assert b"\r\n" + b"0123456789" * 409 + b"!\r\n>>> " in result
            if detach:
                os.write(keyboard, b"\x1d")
            else:
                os.write(keyboard, b"while True: pass\r\r")
                time.sleep(0.05)
                os.set_blocking(keyboard, False)
                # Fill the application's input queue and pause the CLI sender.
                # Ctrl+X must still reach the supervisor rather than that queue.
                for _ in range(32):
                    try:
                        os.write(keyboard, b"x" * 256)
                    except BlockingIOError:
                        break
                time.sleep(0.05)
                os.write(keyboard, b"\x18")
                received(b"brickphyr> ")
                os.write(keyboard, b"app status\r")
                assert b"Application: stopped" in received(b"brickphyr> ")
                os.write(keyboard, b"\x1d")
            assert process.wait(timeout=2) == 0
            assert termios.tcgetattr(tty) == original
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=2)
            os.close(keyboard)
            os.close(tty)

        async def verify():
            async with connection(port=port) as (client, _):
                try:
                    state = await checked(client, Status())
                    assert state.state == (1 if detach else 3) and state.repl
                    if detach:
                        await checked(client, Repl())
                        await checked(client, Input(data=b"answer\r"))
                        assert b"\r\n42\r\n" in await until(client, b">>> ")
                finally:
                    await checked(client, Stop())

        asyncio.run(verify())
