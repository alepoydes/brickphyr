# SPDX-License-Identifier: Apache-2.0
"""Native Python must remain usable with the Pybricks library and adapter off."""

import asyncio
import time
from pathlib import Path

import cbor2
import pytest
import serial
from pybricksdev.ble.pybricks import (
    PYBRICKS_COMMAND_EVENT_UUID,
    PYBRICKS_HUB_CAPABILITIES_UUID,
    Command,
    StatusFlag,
    unpack_hub_capabilities,
)
from pybricksdev.connections.serial import PybricksHubSerial
from test_native import native_console
from test_python import output
from test_python_shell import received

from brickphyrdev.application import (
    Format,
    Status,
    Stop,
    checked,
    connection,
    install,
)

BUILD = Path(__file__).resolve().parents[1] / "build/python-core-host-tests"


def test_native_python_without_compatibility():
    image = cbor2.dumps(
        {
            "main.py": b"from helper import value\n"
            b"try:\n import pybricks\nexcept ImportError:\n print('core', value)\n"
            b"else:\n raise AssertionError('unexpected library')\n"
            b"input()\n",
            "helper.py": b"value = 42\n",
        }
    )

    async def prepare(device):
        async with connection(port=device) as (client, _):
            previous = (await checked(client, Status())).state
            await install(client, image, "core", Format.FILES)
            assert (await checked(client, Status())).state == previous

    async def probe(device):
        hub = PybricksHubSerial(device)
        try:
            await hub.connect()
            capabilities = unpack_hub_capabilities(
                await hub.read_gatt_char(PYBRICKS_HUB_CAPABILITIES_UUID)
            )
            assert capabilities[0] > 0 and capabilities[1:] == (0, 0, 0)
            assert not hub.status_observable.value & StatusFlag.USER_PROGRAM_RUNNING
            # The management profile may still identify itself, but cannot
            # stop, replace or read the autonomous native Python application.
            for command in (
                bytes([Command.STOP_USER_PROGRAM]),
                bytes([Command.START_REPL]),
                bytes([Command.WRITE_USER_PROGRAM_META, 0, 0, 0, 0]),
            ):
                with pytest.raises(RuntimeError, match="Write failed: 128"):
                    await hub.write_gatt_char(PYBRICKS_COMMAND_EVENT_UUID, command, response=True)
            assert hub.output == []
        finally:
            await hub.disconnect()

    async def inspect(device):
        async with connection(port=device) as (client, _):
            status = await checked(client, Status())
            assert status.state == 1 and status.format == Format.FILES
            assert b"core 42\r\n" in await output(client)
            await checked(client, Stop())

    with native_console(BUILD) as device:
        with serial.Serial(device, timeout=0.05, exclusive=True) as port:
            port.write(b"python\r")
            received(port, b">>> ")
            port.write(b"value = 6 * 7\rprint(value)\r")
            received(port, b"\r\n42\r\n>>> ")
            port.write(b"\x04")
            received(port, b">>> ")
            port.write(b"print('value' in globals())\r")
            received(port, b"\r\nFalse\r\n>>> ")
            port.write(b"\x18")
            received(port, b"brickphyr> ")
        asyncio.run(prepare(device))
        with serial.Serial(device, timeout=0.05, exclusive=True) as port:
            port.write(b"app run core\r")
            assert b"run: 0" in received(port, b"brickphyr> ")
        asyncio.run(inspect(device))
        with serial.Serial(device, timeout=0.05, exclusive=True) as port:
            port.write(b"app run core\r")
            assert b"run: 0" in received(port, b"brickphyr> ")
            port.write(b"host_mode pybricks\r")
            assert b"Pybricks selected" in received(port, b"brickphyr> ")
            port.write(b"detach\r")
            received(port, b"brickphyr> ")
            time.sleep(1.4)  # Fixture's 1.2 s synthetic DTR transition.
        asyncio.run(probe(device))
