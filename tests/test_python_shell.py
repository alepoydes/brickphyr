# SPDX-License-Identifier: Apache-2.0
"""Plain REPL and SMP admission through the real shared host dispatcher on ARM."""

import asyncio
import time
from pathlib import Path

import serial
from pybricksdev.ble.pybricks import SW_REV_UUID, StatusFlag
from pybricksdev.connections.serial import PybricksHubSerial
from test_native import native_console

from brickphyrdev import console
from brickphyrdev.application import Repl, Status, Stop, checked

BUILD = Path(__file__).resolve().parents[1] / "build/python-host-tests"


def received(port, marker):
    result = bytearray()
    deadline = time.monotonic() + 4
    while marker not in result:
        assert time.monotonic() < deadline, (marker, bytes(result))
        result.extend(port.read(max(1, port.in_waiting)))
    return bytes(result)


def test_plain_shell_repl_and_return():
    with (
        native_console(BUILD) as device,
        serial.Serial(device, timeout=0.05, exclusive=True) as port,
    ):
        # Deliberate typeahead: the VM must be ready to interpret Ctrl-C as a
        # line cancellation even before its lower-priority thread first runs.
        port.write(b"python\r\x03print(6 * 7)\r")
        received(port, b"\r\n42\r\n>>> ")
        for data, marker in (
            (b"value = 40\r", b">>> "),
            (b"valu\t + 3\x7f2\r", b"\r\n42\r\n>>> "),
            (b"\x1b[A\r", b"\r\n42\r\n>>> "),
            (
                b"import sys; sys.stdout.buffer.write(bytes([6,9,0,4]))\r",
                b"\x06\x09\x00\x044\r\n>>> ",
            ),
            (b"\x04", b">>> "),
            (b"print('value' in globals())\r", b"\r\nFalse\r\n>>> "),
            (b"\x01", b"raw REPL; CTRL-B to exit\r\n>"),
            (b"print(6 * 7)\x04", b"OK42\r\n\x04\x04>"),
            (b"\x02", b">>> "),
        ):
            port.write(data)
            received(port, marker)
        port.write(b"print('0123456789' * 409 + '!')\r")
        result = received(port, b">>> ")
        assert b"\r\n" + b"0123456789" * 409 + b"!\r\n>>> " in result
        port.write(b"while True: pass\r\r")
        time.sleep(0.05)
        port.write(b"\x03")
        assert b"KeyboardInterrupt" in received(port, b">>> ")
        port.write(b"\x18app run\r")
        result = received(port, b"brickphyr> ")
        assert b"Run:" not in result, result
        port.write(b"app status\r")
        assert b"Application: stopped" in received(port, b"brickphyr> ")


def test_managed_shell_preserves_protocol():
    async def exercise(device):
        result = bytearray()
        async with console.connection(result.extend, management=True, port=device) as (
            port,
            client,
        ):
            assert (await checked(client, Status())).state == 0
            await port.write(b"python\r")
            deadline = asyncio.get_running_loop().time() + 2
            while b"SMP is active" not in result or not result.endswith(b"brickphyr> "):
                assert asyncio.get_running_loop().time() < deadline, bytes(result)
                await asyncio.sleep(0.01)
            assert (await checked(client, Status())).state == 0
            await checked(client, Repl())
            assert (await checked(client, Status())).repl
            await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_plain_repl_detach_and_reattach():
    with (
        native_console(BUILD) as device,
        serial.Serial(device, timeout=0.05, exclusive=True) as port,
    ):
        port.write(b"detach\r")
        assert b"Synthetic DTR cycle scheduled" in received(port, b"brickphyr> ")
        port.write(b"python\r")
        received(port, b">>> ")
        port.write(b"answer = 42\r")
        received(port, b">>> ")
        port.write(b"discarded = ")
        # The fixture cycles the actual dispatcher; the REPL command must return
        # on disconnect so Shell teardown can finish without aborting the VM.
        received(port, b"brickphyr> ")
        port.write(b"python\r")
        received(port, b">>> ")
        port.write(b"print(answer, 'discarded' in globals())\r")
        received(port, b"42 False\r\n>>> ")
        port.write(b"\x18")
        received(port, b"brickphyr> ")


def test_pybricks_connection_on_shared_dispatcher():
    async def exercise(device):
        hub = PybricksHubSerial(device)
        try:
            for _ in range(2):
                await hub.connect()
                assert hub.name == "Brickphyr" and hub.hub_kind == 0x81
                assert await hub.read_gatt_char(SW_REV_UUID) == b"1.7.0"
                assert hub.status_observable.value & StatusFlag.USB_HOST_CONNECTED
                await hub.disconnect()
        finally:
            await hub.disconnect()

    with native_console(BUILD) as device:
        with serial.Serial(device, timeout=0.05, exclusive=True) as port:
            port.write(b"host_mode pybricks\r")
            assert b"Pybricks selected" in received(port, b"brickphyr> ")
            port.write(b"detach\r")
            received(port, b"brickphyr> ")
            # The fixture's 1.2 s DTR timer performs the real mode transition.
            time.sleep(1.4)
        asyncio.run(exercise(device))
