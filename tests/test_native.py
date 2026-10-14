# SPDX-License-Identifier: Apache-2.0
"""Run production components and user threads on an ARM MPU model."""

import asyncio
import os
import re
import selectors
import signal
import struct
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


def incompatible_native(image):
    # Change ELF application metadata, preserving the payload's shape.
    from io import BytesIO
    from elftools.elf.elffile import ELFFile

    section = ELFFile(BytesIO(image)).get_section_by_name(".brickphyr.abi")
    changed = bytearray(image)
    struct.pack_into("<H", changed, section["sh_offset"] + 4, 0xffff)
    return bytes(changed)


def qemu(build):
    cache = subprocess.check_output(["cmake", "-LA", "-N", str(build)], text=True)
    return next(
        line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("QEMU:FILEPATH=")
    )


def test_reserved_slot_names():
    from brickphyrdev.application import installed_name

    for name in ("@0", "@127"):
        assert installed_name(name, allow_slot=True) == name
        with pytest.raises(ValueError):
            installed_name(name)  # Ordinary installation cannot overwrite a slot.
    for name in ("@", "@00", "@01", "@128", "@-1", "@١", "@1/../program"):
        with pytest.raises(ValueError):
            installed_name(name, allow_slot=True)


@pytest.mark.parametrize(
    ("suite", "success"),
    [
        ("native-app-tests", b"NATIVE PASS:"),
        ("native-fpu-tests", b"NATIVE PASS:"),
        ("native-portable-tests", b"NATIVE PASS:"),
        ("native-compact-tests", b"NATIVE PASS:"),
        ("boot-handoff-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("python-core-fpu-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("crypto-heap-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("imu-processing-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("motor-control-arm-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("python-arm-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("python-ble-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("python-network-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("python-core-ble-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("python-slots-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("uart-cmsdk-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("driver-userspace-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("driver-supervisor-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("syscall-frame-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("upstream-userspace", b"PROJECT EXECUTION SUCCESSFUL"),
        ("logging-userspace-tests", b"PROJECT EXECUTION SUCCESSFUL"),
        ("main-userspace-tests", b"main_stack: PASS"),
        ("main-supervisor-tests", b"main_stack: PASS"),
    ],
)
def test_arm_firmware(suite, success):
    build = Path(__file__).resolve().parents[1] / "build" / suite
    # Ask Ninja for just Zephyr's generated runner command, without building its
    # dependencies inside the test timeout. Preserve the board's clock/options.
    command = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", "-s", "zephyr/CMakeFiles/run_qemu"],
        text=True,
    ).strip()
    assert command, f"No QEMU runner in {build}; configure and build the fixture first"
    with subprocess.Popen(
        command,
        shell=True,
        cwd=build,
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
                    # The upstream protection suite deliberately triggers kernel
                    # assertions; its fatal handler and final result own success.
                    if suite != "upstream-userspace":
                        assert b"ASSERTION FAIL" not in output, output.decode()
                    assert b"PROJECT EXECUTION FAILED" not in output, output.decode()
                    assert b"main_stack: FAIL" not in output, output.decode()
        finally:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=3)


@contextmanager
def native_console(build, *, virtual_clock=False):
    # This peer interacts with host Python/PTY I/O. Keep its clock paced against
    # host time; an accelerated idle guest could expire a transfer before the
    # host process is scheduled. Autonomous guest checks use Zephyr's runner.
    # Sampled devices need guest time independent of host scheduling stalls.
    # Use the finest instruction clock, paced for host I/O. This is functional
    # device simulation, not the physical CPU's instruction/cycle timing.
    clock = ["-icount", "shift=0,align=off,sleep=on"] if virtual_clock else []
    cortex_m4 = "CONFIG_CPU_CORTEX_M4=y" in (build / "zephyr/.config").read_text().splitlines()
    process = subprocess.Popen(
        [
            qemu(build),
            "-machine",
            "mps2-an386" if cortex_m4 else "mps2-an385",
            "-cpu",
            "cortex-m4" if cortex_m4 else "cortex-m3",
            "-nographic",
            "-monitor",
            "none",
            "-chardev",
            "pty,id=terminal",
            "-serial",
            "chardev:terminal",
            *clock,
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
            motor = await snapshot(client, "motor.A")
            assert motor.error == -19  # Empty port, not a usable motor.
            assert motor.values["owner"] == motor.values["mode"] == 0
            assert motor.values["valid"] == 1
            assert motor.values["duty_permille"] == motor.values["pending"] == 0
            await upload(client, transport, (examples / "spin.elf").read_bytes())
            await checked(client, Start())
            assert (await snapshot(client, "application")).values["state"] == 1
            hello = (examples / "hello.elf").read_bytes()
            incompatible = incompatible_native(hello)
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


def test_dsp_sdk_example():
    from test_python_hub import shell
    from brickphyrdev import console
    from brickphyrdev.application import Remove, RunStored, install

    build = Path(__file__).resolve().parents[1] / "build/native-dsp-tests"
    image = (build.with_name("native-dsp-examples") / "imu_filter.elf").read_bytes()

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            async def sample(expected):
                data = bytearray()
                async with asyncio.timeout(3):
                    while True:
                        data.extend((await checked(client, Output())).data)
                        matches = re.findall(rb"imu-dsp seq=(\d+) epoch=(\d+) raw=(-?\d+) filtered=(-?\d+) mdps\n", data)
                        if any(int(raw) == int(filtered) == expected for _, _, raw, filtered in matches):
                            break
                        state = await checked(client, Status())
                        assert state.state == 1 and state.fault == state.cleanup == 0, (state, data)
                        await asyncio.sleep(0.01)
                state = await checked(client, Status())
                assert state.image == len(image) <= state.limit == 4096
                assert 0 < state.loaded <= state.heap <= 12288
                assert state.stack == 2048
                assert state.dropped == state.fault == state.cleanup == 0
                return state

            # Registers stand in for an IMU; acquisition, FIR, formatting,
            # MPU execution and both loaders are the production paths.
            await shell(port, captured, b"imu_input 0 0 -1000 0 0 -4096")
            await upload(client, port, image)
            await checked(client, Start())
            ram = await sample(70000)
            await checked(client, Stop())
            ram_done = await checked(client, Status())
            assert ram_done.unused > 0 and ram_done.fault == ram_done.cleanup == 0
            await install(client, image, "imu-filter")
            await shell(port, captured, b"imu_input 0 0 1000 0 0 -4096")
            await checked(client, RunStored(name="imu-filter"))
            stored = await sample(-70000)
            assert (stored.heap, stored.loaded) == (ram.heap, ram.loaded)
            await checked(client, Stop())
            stored_done = await checked(client, Status())
            assert stored_done.unused > 0 and stored_done.fault == stored_done.cleanup == 0
            await checked(client, Remove(name="imu-filter"))
            print(f"DSP example: file {ram.image}, loader {ram.heap}, loaded {ram.loaded}, "
                  f"stack unused RAM/stored {ram_done.unused}/{stored_done.unused}")

    with native_console(build, virtual_clock=True) as port:
        asyncio.run(exercise(port))


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
            incompatible = incompatible_native(hello)
            with pytest.raises(RuntimeError, match="Publish rejected"):
                await install(client, incompatible, "hello")
            assert await client.download_file("/apps/hello") == hello
            # Replacement while counter is running changes only its installed image.
            await install(client, hello, "counter")
            assert (await checked(client, Status())).state == 1
            for _ in range(2):
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
