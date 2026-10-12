# SPDX-License-Identifier: Apache-2.0
"""Passive light and UART color matrix through protected Python and port services."""

import asyncio
import re

from test_native import native_console
from test_python import completed, output
from test_python_hub import BUILD, ready, shell
from test_python_pupdevice import port_matches

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Start, Stop, checked, upload
from brickphyrdev.telemetry import snapshot


def matrix_frame(reply, pixels):
    match = re.search(rb"mode=(\d+) bytes=([0-9a-f]+)", reply)
    assert match, reply
    frame = bytes.fromhex(match[2].decode())
    assert int(match[1]) == 2
    assert len(frame) == 21 and frame[:4] == bytes.fromhex("4600b9e2")
    assert frame[4:20] == pixels + bytes(16 - len(pixels))
    checksum = 0
    for byte in frame[3:]:
        checksum ^= byte
    assert checksum == 0xff


async def motor_matches(client, **fields):
    value = None
    try:
        async with asyncio.timeout(3):
            while True:
                value = await snapshot(client, "motor.A")
                if value.error == 0 and all(value.values.get(key) == expected for key, expected in fields.items()):
                    return value
                await asyncio.sleep(0.005)
    except TimeoutError as error:
        error.add_note(f"Expected motor {fields}, last state: {value}")
        raise


def test_light_and_color_light_matrix():
    light = b"""
from pybricks.pupdevices import Light
from pybricks.parameters import Port
import errno
l = Light(Port.A)
print('ready'); input()
assert l.on(25) is None
print('ready'); input()
assert l.on(150) is None
print('ready'); input()
assert l.on(0) is None
print('ready'); input()
assert l.off() is None
print('ready'); input()
try:
    l.on(60)
except OSError as error:
    assert error.args[0] == errno.ENODEV
else:
    assert False
print('ready'); input()
fresh = Light(Port.A)
assert fresh.on(40) is None
print('ready'); input()
assert False, 'host Stop must release and coast the active light'
"""
    matrix = b"""
from pybricks.pupdevices import ColorLightMatrix
from pybricks.parameters import Port, Color
from pybricks.tools import run_task
m = ColorLightMatrix(Port.A)
async def scalar():
    assert await m.on(Color.RED) is None
run_task(scalar())
print('ready'); input()
colors = (Color.RED, Color.BLUE, Color(14, 100, 95), Color(16, 100, 95),
          Color(120, 29, 15), Color(120, 30, 15), Color.WHITE, Color.NONE,
          Color(240, 100, 0))
assert m.on((color for color in colors)) is None
print('ready'); input()
try:
    m.on((Color.RED for _ in range(8)))
except ValueError:
    pass
else:
    assert False
print('ready'); input()
assert m.off() is None
print('ready'); input()
print('matrix passed')
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await shell(port, captured, b"pup_peer light")
                await port_matches(client, phase=2, type_id=8)
                await upload(client, port, light, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await motor_matches(client, owner=1, mode=0, duty_permille=0)

                for duty in (250, 1000, 0, 0):
                    await checked(client, Input(data=b"\n"))
                    await ready(client)
                    await motor_matches(client, mode=1 if duty else 0, duty_permille=duty)
                    if duty:
                        reply = await shell(port, captured, b"pup_peer pwm")
                        assert f"pwm channel=0 period=1000 pulse={duty}".encode() in reply

                await shell(port, captured, b"pup_peer unplug")
                await port_matches(client, phase=1)
                await shell(port, captured, b"pup_peer light")
                await port_matches(client, phase=2, type_id=8)
                await motor_matches(client, mode=0, duty_permille=0)
                before = await shell(port, captured, b"pup_peer pwm")
                await checked(client, Input(data=b"\n"))
                await ready(client)  # Stale Light.on(60) failed before issuing PWM.
                after = await shell(port, captured, b"pup_peer pwm")
                assert after == before
                await motor_matches(client, mode=0, duty_permille=0)

                await checked(client, Input(data=b"\n"))
                await ready(client)
                await motor_matches(client, owner=1, mode=1, duty_permille=400)
                assert b"pwm channel=0 period=1000 pulse=400" in await shell(port, captured, b"pup_peer pwm")
                await checked(client, Stop())
                result = await completed(client, state=3)
                assert result.cleanup == 0, result
                await motor_matches(client, owner=0, mode=0, duty_permille=0)

                await shell(port, captured, b"pup_peer matrix")
                await port_matches(client, phase=7, type_id=64)
                await upload(client, port, matrix, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                matrix_frame(await shell(port, captured, b"pup_peer output"), bytes([0xa9] * 9))
                await checked(client, Input(data=b"\n"))
                await ready(client)
                expected = bytes((0xa9, 0xa3, 0x99, 0x98, 0x0a, 0x16, 0xaa, 0x0a, 0x03))
                frame = await shell(port, captured, b"pup_peer output")
                matrix_frame(frame, expected)
                await checked(client, Input(data=b"\n"))
                await ready(client)
                assert await shell(port, captured, b"pup_peer output") == frame
                await checked(client, Input(data=b"\n"))
                await ready(client)
                matrix_frame(await shell(port, captured, b"pup_peer output"), bytes(9))
                await checked(client, Input(data=b"\n"))
                result = await completed(client)
                text = await output(client)
                assert result.result == result.fault == result.cleanup == 0, text
                assert text.strip() == b"matrix passed"
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0
            except BaseException as error:
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))
