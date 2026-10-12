# SPDX-License-Identifier: Apache-2.0
"""Python applications through the production supervisor and SMP transport."""

import asyncio
import subprocess
import sys
from pathlib import Path

import pytest
from pybricksdev.compile import compile_file
from test_native import native_console

from brickphyrdev.application import (
    Format,
    FormattedUpload,
    Output,
    Start,
    Status,
    Stop,
    checked,
    connection,
    upload,
)

ROOT = Path(__file__).resolve().parents[1]


async def completed(client, state=2):
    async with asyncio.timeout(3):
        while (status := await checked(client, Status())).state == 1:
            await asyncio.sleep(0.01)
    assert status.state == state and status.cleanup == 0, status
    return status


async def output(client):
    result = bytearray()
    remaining = (await checked(client, Status())).queued
    while remaining > 0:
        data = (await checked(client, Output())).data
        assert data
        result.extend(data)
        remaining -= len(data)
    return bytes(result)


def test_python_lifecycle():
    async def exercise(port):
        async with connection(port=port) as (client, transport):
            assert (await checked(client, Status())).formats == 15
            cases = (
                (b"previous = 9\nprint('first')\n", 0, b"first"),
                (b"print(previous)\n", -8, b"NameError"),
                (b"if :\n", -8, b"SyntaxError"),
                (b"1 / 0\n", -8, b"ZeroDivisionError"),
                (b"bytearray(1000000)\n", -8, b"MemoryError"),
                (
                    b"def recurse(): return recurse()\n"
                    b"try: recurse()\nexcept RuntimeError: print('stack limit caught')\n",
                    0,
                    b"stack limit caught",
                ),
                (
                    b"import gc\ndata = [str(n) for n in range(100)]\ngc.collect()\n"
                    b"print(data[99])\n",
                    0,
                    b"99",
                ),
            )
            try:
                for source, expected_result, text in cases:
                    await upload(client, transport, source, Format.PYTHON)
                    await checked(client, Start())
                    status = await completed(client)
                    assert status.result == expected_result, status
                    assert status.format == Format.PYTHON and status.image == len(source)
                    assert status.unused > 0, status
                    assert text in await output(client)
                bytecode = await compile_file(str(ROOT / "tests/python"), "calculation.py", 6)
                await upload(client, transport, bytecode, Format.MPY)
                await checked(client, Start())
                status = await completed(client)
                assert status.result == 0 and status.format == Format.MPY, status
                assert b"6765" in await output(client)
                await upload(client, transport, bytecode[:-1], Format.MPY)
                await checked(client, Start())
                assert (await completed(client)).result == -8
                assert b"truncated .mpy" in await output(client)
                await upload(client, transport, b"for n in range(300): print(n)\n", Format.PYTHON)
                await checked(client, Start())
                status = await completed(client)
                assert status.result == 0 and status.dropped > 0 and status.queued <= 1024
                # A maximum-size source still has heap for compilation and execution.
                source = b"print('maximum source')\n#".ljust(status.limit, b" ")
                await upload(client, transport, source, Format.PYTHON)
                await checked(client, Start())
                assert (await completed(client)).result == 0
                assert b"maximum source" in await output(client)
            finally:
                await checked(client, Stop())

    with native_console(ROOT / "build/python-smp-tests") as port:
        asyncio.run(exercise(port))


def test_python_disconnect_and_stop():
    async def exercise(port):
        async with connection(port=port) as (client, transport):
            await upload(client, transport, b"while True: pass\n", Format.PYTHON)
            await checked(client, Start())
            assert (await checked(client, Status())).state == 1
            # Neither rejected data nor an incomplete upload may stop the VM.
            with pytest.raises(RuntimeError, match="FormattedUpload rejected"):
                await checked(client, FormattedUpload(off=0, total=4, data=b"bad!", format=99))
            for image, format in ((b"x\0y", Format.PYTHON), (b"M\x07\0\x1f", Format.MPY)):
                with pytest.raises(RuntimeError, match="FormattedUpload rejected"):
                    await upload(client, transport, image, format)
                assert (await checked(client, Status())).state == 1
            await checked(client, FormattedUpload(off=0, total=10, data=b"print", format=1))
            with pytest.raises(RuntimeError, match="FormattedUpload rejected"):
                await checked(client, FormattedUpload(off=5, total=10, data=b"x", format=2))
        async with connection(port=port) as (client, transport):
            try:
                status = await checked(client, Status())
                # QEMU's plain UART has no USB/BLE detach indication. Session
                # cleanup is checked separately by the native lifecycle fixture.
                assert status.state == 1 and not status.ready
                await checked(client, Stop())
                status = await checked(client, Status())
                assert status.state == 3 and status.cleanup == 0
                await upload(client, transport, b"print('after stop')\n", Format.PYTHON)
                await checked(client, Start())
                assert (await completed(client)).result == 0
                assert b"after stop" in await output(client)
            finally:
                await checked(client, Stop())

    with native_console(ROOT / "build/python-smp-tests") as port:
        asyncio.run(exercise(port))


def test_engine_replacement():
    build = ROOT / "build/python-smp-tests"

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            for _ in range(3):
                for format, image, state, text in (
                    (Format.NATIVE, (build / "hello.elf").read_bytes(), 2, b"Hello from a native"),
                    (Format.PYTHON, b"print('Python after native')\n", 2, b"Python after native"),
                    (Format.NATIVE, (build / "fault.elf").read_bytes(), 4, b""),
                    (Format.PYTHON, b"print('Python after fault')\n", 2, b"Python after fault"),
                ):
                    await upload(client, transport, image, format)
                    await checked(client, Start())
                    status = await completed(client, state)
                    assert status.format == format
                    assert text in await output(client)
                    if state == 2:
                        assert status.result == 0
            await upload(client, transport, b"while True: pass\n", Format.PYTHON)
            await checked(client, Start())
            await upload(client, transport, (build / "hello.elf").read_bytes())
            assert (await checked(client, Status())).state == 1
            await checked(client, Start())
            assert (await completed(client)).result == 0

    with native_console(build) as port:
        asyncio.run(exercise(port))


def test_python_cli(tmp_path):
    source = tmp_path / "hello.py"
    source.write_text("print('Python CLI works')\n")
    with native_console(ROOT / "build/python-smp-tests") as port:
        cli = [Path(sys.executable).with_name("brickphyrdev"), "app", "--port", port]
        for operation, expected in (
            (["run", str(source)], '"format": "python"'),
            (["status"], '"state": "returned"'),
            (["output"], "Python CLI works"),
        ):
            result = subprocess.run([*cli, *operation], capture_output=True, text=True, timeout=5)
            assert result.returncode == 0 and expected in result.stdout, (
                result.stdout + result.stderr
            )
