# SPDX-License-Identifier: Apache-2.0
"""Passive DC motor commands through protected Python and the native port worker."""

import asyncio

from test_native import native_console
from test_python import completed
from test_python_accessories import motor_matches
from test_python_hub import BUILD, ready, shell
from test_python_pupdevice import port_matches

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Start, Stop, checked, upload


async def driven_duty(client, low, high):
    value = await motor_matches(client, owner=1, mode=1)
    duty = value.values["duty_permille"]
    assert low <= duty <= high, value
    return duty


def test_dcmotor_output_settings_hotplug_and_cleanup():
    source = b"""
from pybricks.iodevices import DCMotor
from pybricks.pupdevices import DCMotor as PUPDCMotor
from pybricks.parameters import Port, Direction
import errno

assert DCMotor is PUPDCMotor

def no_device(call):
    try:
        call()
    except OSError as error:
        assert error.args[0] == errno.ENODEV, error
    else:
        assert False, 'stale motor accepted a command'

m = DCMotor(Port.A, positive_direction=Direction.COUNTERCLOCKWISE)
alias = DCMotor(Port.A, Direction.COUNTERCLOCKWISE)
assert m.settings() == (9000,)
assert m.dc(200) is None
print('ready'); input()

assert alias.settings(1000) is None
assert m.settings() == (1000,)
print('ready'); input()

assert m.dc(100) is None
print('ready'); input()

assert m.brake() is None
print('ready'); input()

assert m.stop() is None
print('ready'); input()

assert m.close() is None
print('ready'); input()

no_device(lambda: m.dc(20))
assert m.close() is None
old = DCMotor(Port.A)
assert old.dc(40) is None
print('ready'); input()

fresh = DCMotor(Port.A)
assert fresh.dc(25) is None
no_device(lambda: old.dc(100))
no_device(old.close)
print('ready'); input()
assert False, 'host Stop must coast the active motor'
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(
            captured.extend, port=device, management=True
        ) as (port, client):
            try:
                await shell(port, captured, b"pup_peer motor")
                await port_matches(client, phase=2, type_id=2)
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await motor_matches(client, owner=1, mode=1, duty_permille=-1000)
                pwm = await shell(port, captured, b"pup_peer pwm")
                assert b"pwm channel=1 period=1000 pulse=1000" in pwm

                await checked(client, Input(data=b"\n"))
                await ready(client)
                await motor_matches(client, owner=1, mode=1, duty_permille=-1000)
                pwm = await shell(port, captured, b"pup_peer pwm")
                assert b"pwm channel=1 period=1000 pulse=1000" in pwm

                await checked(client, Input(data=b"\n"))
                await ready(client)
                duty = await driven_duty(client, -180, -100)
                pwm = await shell(port, captured, b"pup_peer pwm")
                assert f"pwm channel=1 period=1000 pulse={-duty}".encode() in pwm

                await checked(client, Input(data=b"\n"))
                await ready(client)
                await motor_matches(client, owner=1, mode=1, duty_permille=0)

                await checked(client, Input(data=b"\n"))
                await ready(client)
                await motor_matches(client, owner=1, mode=0, duty_permille=0)

                await checked(client, Input(data=b"\n"))
                await ready(client)
                await motor_matches(client, owner=0, mode=0, duty_permille=0)
                await port_matches(client, phase=2, type_id=2)

                await checked(client, Input(data=b"\n"))
                await ready(client)
                await driven_duty(client, 399, 400)

                await shell(port, captured, b"pup_peer unplug")
                await port_matches(client, phase=1)
                await shell(port, captured, b"pup_peer motor")
                await port_matches(client, phase=2, type_id=2)
                await checked(client, Input(data=b"\n"))
                await ready(client)
                duty = await driven_duty(client, 249, 250)
                pwm = await shell(port, captured, b"pup_peer pwm")
                assert f"pwm channel=0 period=1000 pulse={duty}".encode() in pwm

                await checked(client, Stop())
                result = await completed(client, state=3)
                assert result.cleanup == 0, result
                await motor_matches(client, owner=0, mode=0, duty_permille=0)
            except BaseException as error:
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))
