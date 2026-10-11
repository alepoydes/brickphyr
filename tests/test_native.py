# SPDX-License-Identifier: Apache-2.0
"""Run the production loader and user threads on an ARM MPU model."""

import asyncio
import os
import re
import selectors
import signal
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path

import pytest
import serial

from brickphyrdev.application import (
    Output,
    Start,
    Status,
    Stop,
    checked,
    connection,
    upload,
)


def qemu(build):
    cache = subprocess.check_output(["cmake", "-LA", "-N", str(build)], text=True)
    return next(
        line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("QEMU:FILEPATH=")
    )


def test_native_lifecycle():
    build = Path(__file__).resolve().parents[1] / "build/native-app-tests"
    with subprocess.Popen(
        [
            qemu(build),
            "-machine",
            "mps2-an385",
            "-cpu",
            "cortex-m3",
            "-nographic",
            "-monitor",
            "none",
            "-kernel",
            str(build / "zephyr/zephyr.elf"),
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    ) as process:
        try:
            output = bytearray()
            deadline = time.monotonic() + 10
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while b"NATIVE PASS:" not in output:
                    remaining = deadline - time.monotonic()
                    assert remaining > 0 and selector.select(remaining), output.decode()
                    chunk = os.read(process.stdout.fileno(), 4096)
                    assert chunk, output.decode()
                    output.extend(chunk)
                    assert b"NATIVE FAIL" not in output, output.decode()
        finally:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=3)


@contextmanager
def native_console(build):
    process = subprocess.Popen(
        [
            qemu(build),
            "-machine",
            "mps2-an385",
            "-cpu",
            "cortex-m3",
            "-nographic",
            "-monitor",
            "none",
            "-chardev",
            "pty,id=terminal",
            "-serial",
            "chardev:terminal",
            "-kernel",
            str(build / "zephyr/zephyr.elf"),
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    try:
        output = bytearray()
        deadline = time.monotonic() + 3
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while not (match := re.search(rb"redirected to (\S+)", output)):
                left = deadline - time.monotonic()
                assert left > 0 and selector.select(left), output.decode()
                data = os.read(process.stdout.fileno(), 4096)
                assert data, output.decode()
                output.extend(data)
        port = match[1].decode()
        with serial.Serial(port, timeout=2, write_timeout=1, exclusive=True) as console:
            console.write(b"\r")
            assert console.read_until(b"brickphyr> ").endswith(b"brickphyr> ")
        yield port
    finally:
        process.terminate()
        process.wait(timeout=3)
        process.stdout.close()


def test_native_smp_and_cli():
    build = Path(__file__).resolve().parents[1] / "build/native-smp-tests"
    examples = build.with_name("native-smp-examples")

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            await upload(client, transport, (examples / "hello.elf").read_bytes())
            status = await checked(client, Status())
            assert status.ready and status.state == 0
            await checked(client, Start())
            async with asyncio.timeout(2):
                while (status := await checked(client, Status())).state == 1:
                    await asyncio.sleep(0.01)
            assert status.state == 2 and status.result == status.cleanup == 0
            assert b"Hello from a native application." in (await checked(client, Output())).data
            await upload(client, transport, (examples / "spin.elf").read_bytes())
            await checked(client, Start())
            hello = (examples / "hello.elf").read_bytes()
            incompatible = hello.replace(status.abi.encode(), b"0" * 64)
            for bad in (hello[:-1], incompatible):
                with pytest.raises(RuntimeError, match="Upload rejected"):
                    await upload(client, transport, bad)
                assert (await checked(client, Status())).state == 1
            await checked(client, Stop())
            await upload(client, transport, (examples / "fault.elf").read_bytes())
            await checked(client, Start())
            async with asyncio.timeout(2):
                while (status := await checked(client, Status())).state == 1:
                    await asyncio.sleep(0.01)
            assert status.state == 4 and status.cleanup == 0

    with native_console(build) as port:
        asyncio.run(exercise(port))
        cli = [Path(sys.executable).with_name("brickphyrdev"), "app", "--port", port]
        for operation, expected in (
            (["run", str(examples / "counter.elf")], '"state": "running"'),
            (["output"], "counter 00000000 buttons 5"),
            (["stop"], '"state": "stopped"'),
        ):
            result = subprocess.run([*cli, *operation], capture_output=True, text=True, timeout=5)
            assert result.returncode == 0 and expected in result.stdout, (
                result.stdout + result.stderr
            )
