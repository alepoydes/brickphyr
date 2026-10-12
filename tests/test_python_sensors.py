# SPDX-License-Identifier: Apache-2.0
"""Wired sensor bindings through protected Python and the native UART service."""

import asyncio

from test_native import native_console
from test_python import completed, output
from test_python_hub import BUILD, ready, shell
from test_python_pupdevice import port_matches

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Start, checked, upload
from brickphyrdev.telemetry import snapshot


def test_force_tilt_infrared_sensors():
    force = b"""
from pybricks.pupdevices import ForceSensor
from pybricks.parameters import Port
from pybricks.tools import run_task
d = ForceSensor(Port.A)
assert isinstance(d.force(), float) and d.force() == 4.5
assert abs(d.distance() - 3.335) < 0.001
assert d.touched() is True
assert d.pressed() is True
assert d.pressed(force=4.5) is False
async def check():
    assert await d.force() == 4.5
run_task(check())
print('force passed')
"""
    tilt = b"""
from pybricks.pupdevices import TiltSensor
from pybricks.parameters import Port
from pybricks.tools import run_task
d = TiltSensor(Port.A)
assert d.tilt() == (30, -20)
async def check():
    assert await d.tilt() == (30, -20)
run_task(check())
print('tilt passed')
"""
    infrared = b"""
from pybricks.pupdevices import InfraredSensor
from pybricks.parameters import Port
from pybricks.tools import run_task
import errno
d = InfraredSensor(Port.A)
assert d.count() == 5  # Constructor stored the initial count of 41.
assert d.distance() == 10  # Relative percentage, not millimetres.
assert d.reflection() == 20.0
async def check():
    assert await d.count() == 5
run_task(check())
print('ready')
input()
try:
    d.distance()
except OSError as error:
    assert error.args[0] == errno.ENODEV
else:
    assert False
print('infrared passed')
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                for kind, source, expected in (
                    (b"force", force, b"force passed"),
                    (b"tilt", tilt, b"tilt passed"),
                ):
                    await shell(port, captured, b"pup_peer " + kind)
                    # Constructor is the first sensor read; no host warm-up.
                    await upload(client, port, source, Format.PYTHON)
                    await checked(client, Start())
                    result = await completed(client)
                    text = await output(client)
                    assert result.result == result.fault == result.cleanup == 0, text
                    assert text.strip() == expected
                    assert (await snapshot(client, "sensor.A")).values["owner"] == 0
                    await shell(port, captured, b"pup_peer unplug")
                    await port_matches(client, phase=1)

                await shell(port, captured, b"pup_peer infrared")
                await upload(client, port, infrared, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await shell(port, captured, b"pup_peer unplug")
                await port_matches(client, phase=1)
                await checked(client, Input(data=b"\n"))
                result = await completed(client)
                text = await output(client)
                assert result.result == result.fault == result.cleanup == 0, text
                assert text.strip() == b"infrared passed"
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0
            except BaseException as error:
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))
