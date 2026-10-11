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
from brickphyrdev.telemetry import info, snapshot


def qemu(build):
    cache = subprocess.check_output(["cmake", "-LA", "-N", str(build)], text=True)
    return next(
        line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("QEMU:FILEPATH=")
    )


@pytest.mark.parametrize(
    ("suite", "success"),
    [
        ("native-app-tests", b"NATIVE PASS:"),
        ("imu-processing-tests", b"PROJECT EXECUTION SUCCESSFUL"),
    ],
)
def test_arm_firmware(suite, success):
    build = Path(__file__).resolve().parents[1] / "build" / suite
    # Use Zephyr's runner, including the board's instruction-count clock.
    # A hand-written QEMU command silently ignored CONFIG_QEMU_ICOUNT and made
    # guest deadlines depend on host scheduling and translation overhead.
    with subprocess.Popen(
        ["cmake", "--build", str(build), "--target", "run"],
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
                while success not in output:
                    remaining = deadline - time.monotonic()
                    assert remaining > 0 and selector.select(remaining), output.decode()
                    chunk = os.read(process.stdout.fileno(), 4096)
                    assert chunk, output.decode()
                    output.extend(chunk)
                    assert b"NATIVE FAIL" not in output, output.decode()
                    assert b"ASSERTION FAIL" not in output, output.decode()
                    assert b"PROJECT EXECUTION FAILED" not in output, output.decode()
        finally:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=3)


@contextmanager
def native_console(build):
    # This peer interacts with host Python/PTY I/O. Keep its clock paced against
    # host time; an accelerated idle guest could expire a transfer before the
    # host process is scheduled. Autonomous guest checks use Zephyr's runner.
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
            assert "application" in (await info(client)).sources
            await upload(client, transport, (examples / "hello.elf").read_bytes())
            status = await checked(client, Status())
            assert status.ready and status.state == 0
            await checked(client, Start())
            async with asyncio.timeout(2):
                while (status := await checked(client, Status())).state == 1:
                    await asyncio.sleep(0.01)
            assert status.state == 2 and status.result == status.cleanup == 0
            observed = await snapshot(client, "application")
            assert observed.error == 0 and observed.values["state"] == 2
            assert observed.values["queued"] > 0
            assert (await snapshot(client, "application")).values == observed.values
            assert b"Hello from a native application." in (await checked(client, Output())).data
            await upload(client, transport, (examples / "sound.elf").read_bytes())
            await checked(client, Start())
            async with asyncio.timeout(2):
                while (status := await checked(client, Status())).state == 1:
                    await asyncio.sleep(0.01)
            assert status.state == 2 and status.result == status.cleanup == 0
            sound = await snapshot(client, "sound")
            battery = await snapshot(client, "battery")
            assert battery.error == 0
            assert battery.values["current_ma"] == 1825
            assert battery.values["voltage_mv"] == 7767
            assert battery.values["sequence"] > 0
            assert sound.error == 0 and sound.values["owner"] == 0
            assert sound.values["completed"] == 8192
            async with asyncio.timeout(5):
                while (port_a := await snapshot(client, "port.A")).error == -11:
                    await asyncio.sleep(0.01)
            assert port_a.error == 0
            assert port_a.values["type_id"] == 0 and port_a.values["phase"] == 1
            assert port_a.values["sequence"] > 0
            assert (await snapshot(client, "port.F")).values["type_id"] == 0
            await upload(client, transport, (examples / "spin.elf").read_bytes())
            await checked(client, Start())
            assert (await snapshot(client, "application")).values["state"] == 1
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


def test_installed_applications():
    from smpclient.requests.file_management import FileClose, FileUpload

    from brickphyrdev.application import (
        ListFiles,
        Publish,
        Remove,
        RunStored,
        files,
        install,
    )

    build = Path(__file__).resolve().parents[1] / "build/native-smp-tests"
    examples = build.with_name("native-smp-examples")
    hello = (examples / "hello.elf").read_bytes()
    counter = (examples / "counter.elf").read_bytes()

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            assert not (await checked(client, ListFiles(name=""))).name
            await install(client, hello, "hello")
            await install(client, counter, "counter")
            assert (await checked(client, Status())).state == 0, "Installation executed code"
            assert [x.name async for x in files(client)] == ["counter", "hello"]
            assert await client.download_file("/apps/hello") == hello
            await checked(client, RunStored(name="counter"))
            assert (await checked(client, Status())).state == 1
            for path in ("/settings/values", "/apps/../settings/values", "/apps/hello"):
                with pytest.raises(RuntimeError, match="FileUpload rejected"):
                    await checked(client, FileUpload(name=path, off=0, len=1, data=b"x"))
            # A truncated transfer must not replace the installed version or stop
            # its running instance. Publication closes the standard open handle.
            await checked(
                client, FileUpload(name="/apps/.incoming", off=0, len=len(hello), data=hello[:64])
            )
            with pytest.raises(RuntimeError, match="Publish rejected"):
                await checked(client, Publish(name="hello", size=len(hello)))
            assert await client.download_file("/apps/hello") == hello
            assert (await checked(client, Status())).state == 1
            incompatible = hello.replace((await checked(client, Status())).abi.encode(), b"0" * 64)
            with pytest.raises(RuntimeError, match="Publish rejected"):
                await install(client, incompatible, "hello")
            assert await client.download_file("/apps/hello") == hello
            # Replacement while counter is running changes only its installed image.
            await install(client, hello, "counter")
            assert (await checked(client, Status())).state == 1
            for _ in range(3):
                await checked(client, RunStored(name="hello"))
                async with asyncio.timeout(2):
                    while (status := await checked(client, Status())).state == 1:
                        await asyncio.sleep(0.01)
                assert status.state == 2 and status.result == status.cleanup == 0
            await upload(client, transport, hello)
            await checked(client, Start())
            async with asyncio.timeout(2):
                while (ram := await checked(client, Status())).state == 1:
                    await asyncio.sleep(0.01)
            assert (ram.heap, ram.loaded, ram.image) == (status.heap, status.loaded, status.image)
            # Standard file close supports an interrupted upload and a fresh one.
            await checked(
                client, FileUpload(name="/apps/.incoming", off=0, len=len(hello), data=b"bad")
            )
            await checked(client, FileClose())
            await install(client, counter, "counter")
            await checked(client, Remove(name="counter"))
            assert [x.name async for x in files(client)] == ["hello"]

    with native_console(build) as port:
        asyncio.run(exercise(port))
        cli = [Path(sys.executable).with_name("brickphyrdev"), "app", "--port", port]
        for operation, expected in (
            (["install", str(examples / "counter.elf")], '"state": "returned"'),
            (["list"], "counter\t"),
            (["start", "counter"], '"state": "running"'),
            (["remove", "counter"], '"state": "running"'),
            (["stop"], '"state": "stopped"'),
        ):
            result = subprocess.run([*cli, *operation], capture_output=True, text=True, timeout=8)
            assert result.returncode == 0 and expected in result.stdout, (
                result.stdout + result.stderr
            )


def test_application_store_capacity():
    from smpclient.exceptions import SMPUploadError
    from smpclient.requests.file_management import FileClose

    from brickphyrdev.application import ListFiles, install

    build = Path(__file__).resolve().parents[1] / "build/native-smp-tests"
    hello = (build.with_name("native-smp-examples") / "hello.elf").read_bytes()

    async def exercise(port):
        async with connection(port=port) as (client, _):
            await install(client, hello, "original")
            # Synthetic maximum-size files exhaust the real LittleFS allocator.
            padded = hello.ljust((await checked(client, Status())).limit, b"\0")
            for index in range(3):
                try:
                    await install(client, padded, f"fill{index}")
                except SMPUploadError as err:
                    assert index > 0, str(err)
                    break
            else:
                pytest.fail("Expected the bounded application partition to fill")
            await checked(client, FileClose())
            assert await client.download_file("/apps/original") == hello
            assert (await checked(client, ListFiles(name=""))).available <= len(padded)
            assert (await checked(client, Status())).state == 0

    with native_console(build) as port:
        asyncio.run(exercise(port))
