# SPDX-License-Identifier: Apache-2.0
"""Color and light sensor APIs through protected Python and the UART peer."""

import asyncio
import re

from test_native import native_console
from test_python import completed, output
from test_python_hub import BUILD, ready, shell
from test_python_pupdevice import port_matches

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Start, checked, upload
from brickphyrdev.telemetry import snapshot


def output_frame(reply):
    match = re.search(rb"mode=(\d+) bytes=([0-9a-f]+)", reply)
    assert match, reply
    return int(match[1]), bytes.fromhex(match[2].decode())


def test_color_ultrasonic_and_color_distance():
    color = b"""
from pybricks.pupdevices import ColorSensor
from pybricks.parameters import Port, Color
from pybricks.tools import run_task

c = ColorSensor(Port.A)
assert c.reflection() == 33.3
assert c.ambient() == 10.0
surface = c.hsv()
assert (surface.h, surface.s, surface.v) == (10, 100, 100)
ambient = c.hsv(surface=False)
assert (ambient.h, ambient.s, ambient.v) == (120, 100, 100)
assert c.color() == Color.RED
assert c.color(surface=False) == Color.GREEN
defaults = (Color.RED, Color.YELLOW, Color.GREEN, Color.BLUE, Color.WHITE, Color.NONE)
assert c.detectable_colors() == defaults
c.detectable_colors([])
assert c.color() is None
palette = [Color.RED, Color.BLUE]
c.detectable_colors(palette)
palette[0] = Color.GREEN
assert c.detectable_colors() == (Color.RED, Color.BLUE)
other = ColorSensor(Port.A)
assert c.detectable_colors() == other.detectable_colors() == defaults
c.detectable_colors((Color.BLUE, Color.RED))
assert other.detectable_colors() == (Color.BLUE, Color.RED)
try:
    c.detectable_colors((Color.RED,) * 9)
except ValueError:
    pass
else:
    assert False
assert other.detectable_colors() == (Color.BLUE, Color.RED)
custom = Color(10, 80, 90)
c.detectable_colors((custom, Color.BLUE))
assert c.color() == custom

async def check():
    assert tuple(await c.hsv(surface=False)) == (120, 100, 100)
    assert await c.lights.on((10, 20, 30)) is None
run_task(check())
print('ready')
input()
c.lights.on(150)  # Percentages above 100 are clipped.
print('ready')
input()
c.lights.off()
print('ready')
input()
print('color passed')
"""
    ultrasonic = b"""
from pybricks.pupdevices import UltrasonicSensor
from pybricks.parameters import Port
from pybricks.tools import run_task

u = UltrasonicSensor(Port.A)
assert u.distance() == 321
assert u.presence() is True
async def check():
    assert await u.lights.on((1, 2, 3, 4)) is None
run_task(check())
print('ready')
input()
u.lights.off()
print('ultrasonic passed')
"""
    color_distance = b"""
from pybricks.pupdevices import ColorDistanceSensor
from pybricks.parameters import Port, Color

d = ColorDistanceSensor(Port.A)
assert d.distance() == 70  # Relative percentage, not millimetres.
assert d.ambient() == 25.0
assert abs(d.reflection() - 33.3) < 0.1
assert d.hsv().h == 10
assert d.color() == Color.RED
assert d.detectable_colors() == (
    Color.RED, Color.YELLOW, Color.GREEN, Color.BLUE, Color.WHITE, Color.NONE)
d.light.on(Color.BLUE)
print('ready')
input()
d.light.off()
print('ready')
input()
print('color distance passed')
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                cases = (
                    (b"color", 61, color, (
                        (3, bytes.fromhex("4600b9d30a141e002c")),
                        (3, bytes.fromhex("4600b9d36464640048")),
                        (3, bytes.fromhex("4600b9d3000000002c")),
                    ), b"color passed"),
                    (b"ultrasonic", 62, ultrasonic, (
                        (5, bytes.fromhex("4600b9d5010203042e")),
                    ), b"ultrasonic passed"),
                    (b"color_distance", 37, color_distance, (
                        (5, bytes.fromhex("4600b9c50339")),
                        (5, bytes.fromhex("4600b9c5003a")),
                    ), b"color distance passed"),
                )
                for kind, type_id, source, frames, expected in cases:
                    await shell(port, captured, b"pup_peer " + kind)
                    # Cached detection is read-only; the constructor does the first sensor read.
                    await port_matches(client, phase=7, type_id=type_id)
                    await upload(client, port, source, Format.PYTHON)
                    await checked(client, Start())
                    for frame in frames:
                        await ready(client)
                        assert output_frame(await shell(port, captured, b"pup_peer output")) == frame
                        await checked(client, Input(data=b"\n"))
                    result = await completed(client)
                    text = await output(client)
                    assert result.result == result.fault == result.cleanup == 0, text
                    assert text.strip() == expected
                    assert (await snapshot(client, "sensor.A")).values["owner"] == 0
                    await shell(port, captured, b"pup_peer unplug")
                    await port_matches(client, phase=1)
            except BaseException as error:
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))
