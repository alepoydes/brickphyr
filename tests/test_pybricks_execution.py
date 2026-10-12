# SPDX-License-Identifier: Apache-2.0
"""The actual pybricksdev execution workflow against protected ARM firmware."""

import asyncio
import struct
import time
from contextlib import contextmanager
from pathlib import Path

import pytest
import serial
from pybricksdev.ble.pybricks import PYBRICKS_COMMAND_EVENT_UUID, Command, StatusFlag
from pybricksdev.compile import compile_file, compile_multi_file
from pybricksdev.connections.serial import PybricksHubSerial
from test_native import native_console
from test_python_shell import received

BUILD = Path(__file__).resolve().parents[1] / "build/python-host-tests"


async def stopped(hub):
    done = asyncio.Event()
    with hub.status_observable.subscribe(
        lambda flags: done.set() if not flags & StatusFlag.USER_PROGRAM_RUNNING else None
    ):
        await asyncio.wait_for(done.wait(), 3)


@contextmanager
def pybricks_port(slot=None):
    with native_console(BUILD) as device:
        with serial.Serial(device, timeout=0.05, exclusive=True) as port:
            if slot is not None:
                port.write(f"pybricks_slot {slot}\r".encode())
                assert f"Pybricks slot: {slot} of".encode() in received(port, b"brickphyr> ")
            port.write(b"host_mode pybricks\r")
            assert b"Pybricks selected" in received(port, b"brickphyr> ")
            port.write(b"detach\r")
            received(port, b"brickphyr> ")
            time.sleep(1.4)  # Fixture's 1.2 s synthetic DTR timer.
        yield device


def test_pybricks_upload_imports_and_repeat(tmp_path):
    source = tmp_path / "program.py"
    source.write_text(
        "import main\nfrom pkg.helper import calculate\nprint(calculate(main.value))\n"
    )
    (tmp_path / "main.py").write_text("value = 21\n")
    (tmp_path / "pkg").mkdir()
    (tmp_path / "pkg/__init__.py").write_text("factor = 2\n")
    (tmp_path / "pkg/helper.py").write_text("import pkg\ndef calculate(n): return n * pkg.factor\n")

    async def exercise(port):
        hub = PybricksHubSerial(port)
        try:
            await hub.connect()
            states = []
            with hub.status_observable.subscribe(lambda flags: states.append(flags)):
                await asyncio.wait_for(hub.run(str(source), print_output=False), 8)
            assert hub.output == [b"42"]
            assert any(s & StatusFlag.USER_PROGRAM_RUNNING for s in states)
            assert not states[-1] & StatusFlag.USER_PROGRAM_RUNNING
            # Client run(None) repeats the downloaded program without uploading.
            await asyncio.wait_for(hub.run(print_output=False), 4)
            assert hub.output == [b"42"]
            await hub.disconnect()
            await hub.connect()
            await asyncio.wait_for(hub.run(print_output=False), 4)
            assert hub.output == [b"42"]
        finally:
            await hub.disconnect()

    with pybricks_port() as port:
        asyncio.run(exercise(port))


def test_tools_example_through_client():
    root = Path(__file__).resolve().parents[1]

    async def exercise(port):
        hub = PybricksHubSerial(port)
        try:
            await hub.connect()
            await asyncio.wait_for(
                hub.run(str(root / "examples/python/tools.py"), print_output=False), 8
            )
            assert hub.output[:2] == [
                b"Cross product: (0.0, 0.0, 1.0)",
                b"Half brightness: (60, 100, 50)",
            ]
            assert hub.output[-1] == b"Completed: ('first', 'second')"
        finally:
            await hub.disconnect()

    with pybricks_port() as port:
        asyncio.run(exercise(port))


def test_pybricks_input_stop_and_invalid_transfer(tmp_path):
    source = tmp_path / "program.py"
    source.write_text("print('ready')\nprint('hello', input())\n")

    async def exercise(port):
        hub = PybricksHubSerial(port)

        async def command(data):
            await hub.write_gatt_char(PYBRICKS_COMMAND_EVENT_UUID, data, response=True)

        try:
            await hub.connect()
            await hub.run(str(source), wait=False, print_output=False)
            assert await asyncio.wait_for(hub.read_line(), 2) == "ready"
            await command(bytes([Command.WRITE_STDIN]) + b"Brickphyr\n")
            # input() echoes through MicroPython readline before the result.
            while await asyncio.wait_for(hub.read_line(), 2) != "hello Brickphyr":
                pass
            await stopped(hub)
            source.write_text("print('loop ready')\nwhile True: pass\n")
            await hub.run(str(source), wait=False, print_output=False)
            assert await asyncio.wait_for(hub.read_line(), 2) == "loop ready"
            with pytest.raises(RuntimeError, match="Write failed: 129"):
                await hub.start_user_program()
            with pytest.raises(RuntimeError, match="Write failed: 129"):
                await command(struct.pack("<BI", Command.WRITE_USER_PROGRAM_META, 0))
            await hub.stop_user_program()
            await stopped(hub)
            for invalid in (b"\x03", b"\x04\x00", b"\x01\x00\x00"):
                with pytest.raises(RuntimeError, match="Write failed"):
                    await command(invalid)
            source.write_text("print('recovered')\n")
            package = await compile_multi_file(str(source), 6)
            with pytest.raises(RuntimeError, match="Write failed"):
                await hub.download_user_program(package[:-1])
            await asyncio.wait_for(hub.run(str(source), print_output=False), 6)
            assert hub.output == [b"recovered"]
        finally:
            await hub.disconnect()

    with pybricks_port() as port:
        asyncio.run(exercise(port))


def test_hub_example_through_client():
    root = Path(__file__).resolve().parents[1]

    async def exercise(port):
        hub = PybricksHubSerial(port)
        try:
            await hub.connect()
            await asyncio.wait_for(
                hub.run(str(root / "examples/python/hub.py"), print_output=False), 8
            )
            assert hub.output == [b"Buttons: 2", b"Display complete"]
        finally:
            await hub.disconnect()

    with pybricks_port() as port:
        asyncio.run(exercise(port))


def test_pybricks_package_admission(tmp_path):
    source = tmp_path / "program.py"
    source.write_text("print('package recovered')\n")

    async def exercise(port):
        module = await compile_file(str(tmp_path), source.name, 6)

        def record(name, data=module):
            return struct.pack("<I", len(data)) + name.encode() + b"\0" + data

        entry = record("__main__")
        invalid = [
            record("missing_entry"),
            entry + entry,
            entry + record("pkg..child"),
            entry + record(".escape"),
            entry + record("bad", module[:1] + b"\x07" + module[2:]),
            entry + b"\xff\xff\xff\xffmissing-nul",
            entry + b"\xff\xff\xff\xffoverflow\0" + module,
            entry + b"".join(record(f"m{i}") for i in range(64)),
        ]
        hub = PybricksHubSerial(port)
        try:
            await hub.connect()
            for package in invalid:
                with pytest.raises(RuntimeError, match="Write failed"):
                    await hub.download_user_program(package)
            await asyncio.wait_for(hub.run(str(source), print_output=False), 6)
            assert hub.output == [b"package recovered"]
        finally:
            await hub.disconnect()

    with pybricks_port() as port:
        asyncio.run(exercise(port))


def test_pybricks_long_output(tmp_path):
    source = tmp_path / "program.py"
    source.write_text("print('0123456789' * 409 + '!')\n")

    async def exercise(port):
        hub = PybricksHubSerial(port)
        try:
            await hub.connect()
            await asyncio.wait_for(hub.run(str(source), print_output=False), 8)
            assert hub.output == [b"0123456789" * 409 + b"!"]
        finally:
            await hub.disconnect()

    with pybricks_port() as port:
        asyncio.run(exercise(port))


def test_pybricks_selected_slot_and_rejected_replacement(tmp_path):
    source = tmp_path / "program.py"
    source.write_text(
        "from pybricks.hubs import PrimeHub\n"
        "info = PrimeHub().system.info()\n"
        "assert info['program_id'] == 2 and info['program_start_type'] == 3\n"
        "assert info['host_connected_usb'] and not info['host_connected_ble']\n"
        "print('stored in slot 2')\n"
    )

    async def exercise(port):
        hub = PybricksHubSerial(port)
        try:
            await hub.connect()
            await asyncio.wait_for(hub.run(str(source), print_output=False), 8)
            assert hub.output == [b"stored in slot 2"]
            package = await compile_multi_file(str(source), 6)
            with pytest.raises(RuntimeError, match="Write failed"):
                await hub.download_user_program(package[:-1])
            await asyncio.wait_for(hub.run(print_output=False), 4)
            assert hub.output == [b"stored in slot 2"]
            # No upload remains after atomic publication. An explicit slot start
            # must load the store, and an empty slot must not run another image.
            await hub.start_user_program(2)
            assert await asyncio.wait_for(hub.read_line(), 2) == "stored in slot 2"
            await stopped(hub)
            with pytest.raises(RuntimeError, match="Write failed"):
                await hub.start_user_program(1)
            await asyncio.wait_for(hub.run(print_output=False), 4)
            assert hub.output == [b"stored in slot 2"]
        finally:
            await hub.disconnect()

    with pybricks_port(slot=2) as port:
        asyncio.run(exercise(port))


def test_pybricks_native_modules(tmp_path):
    source = tmp_path / "program.py"
    source.write_text(
        "import gc, micropython, native_module\nfrom fast import calculate\n"
        "@micropython.native\ndef add(a, b): return a + b\n"
        "gc.collect()\nprint('native result', add(20, 22), calculate(6), native_module.add(30))\n"
    )
    fast = tmp_path / "fast.py"
    fast.write_text(
        "import micropython\nfrom constant import value\n"
        "@micropython.viper\ndef multiply(x: int, y: int) -> int: return x * y\n"
        "def calculate(x): return multiply(x, value)\n"
    )
    (tmp_path / "constant.py").write_text("value = 7\n")
    (tmp_path / "native_module.mpy").write_bytes(
        (BUILD / "python-fixtures/native_module.mpy").read_bytes()
    )

    async def exercise(port):
        native = await compile_file(str(tmp_path), fast.name, (6, 3), ["-march=armv7m"])
        fast.unlink()
        (tmp_path / "fast.mpy").write_bytes(native)
        hub = PybricksHubSerial(port)
        try:
            await hub.connect()
            await asyncio.wait_for(hub.run(str(source), print_output=False), 8)
            assert hub.output == [b"native C 42 1", b"native result 42 42 42"]
            # Rejected compilation must not alter the previous stored program.
            (tmp_path / "fast.mpy").write_bytes(native[:2] + bytes([native[2] ^ 1]) + native[3:])
            with pytest.raises(ValueError, match="abi minor version"):
                await hub.download(str(source))
            await asyncio.wait_for(hub.run(print_output=False), 6)
            assert hub.output == [b"native C 42 1", b"native result 42 42 42"]
        finally:
            await hub.disconnect()

    with pybricks_port() as port:
        asyncio.run(exercise(port))
