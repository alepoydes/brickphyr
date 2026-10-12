# SPDX-License-Identifier: Apache-2.0
"""Power Functions IR writes through the protected Python UART sensor path."""

import asyncio
import re

from test_native import native_console
from test_python import completed
from test_python_color_sensors import output_frame
from test_python_hub import BUILD, ready, shell
from test_python_pupdevice import port_matches

from smpclient import success
from brickphyrdev import console
from brickphyrdev.application import Format, Input, Start, Status, Stop, checked, upload
from brickphyrdev.telemetry import snapshot


def pf_history(reply):
    header = re.search(rb"pf count=(\d+) dropped=(\d+)", reply)
    assert header, reply
    records = [(int(time), int(message, 16)) for time, message in
               re.findall(rb"pf t=(\d+) value=([0-9a-f]{4})", reply)]
    assert len(records) == int(header[1]) and int(header[2]) == 0, reply
    return records


def spaced(records):
    assert all(later[0] - earlier[0] >= 250 for earlier, later in zip(records, records[1:]))


def test_pf_motor_output_and_cleanup():
    source = b"""
from pybricks.pupdevices import ColorDistanceSensor, PFMotor
from pybricks.parameters import Port, Color, Direction
from pybricks.tools import run_task

sensor = ColorDistanceSensor(Port.A)
motors = []
for channel in range(1, 5):
    motors.append(PFMotor(sensor, channel, Color.RED))
    direction = Direction.COUNTERCLOCKWISE if channel == 1 else Direction.CLOCKWISE
    motors.append(PFMotor(sensor, channel, Color.BLUE, positive_direction=direction))

async def first_write():
    assert await motors[0].dc(44) is None
run_task(first_write())
assert sensor.distance() == 70  # A read must reselect the sensor after IR_TX.
assert motors[0].dc(-30) is None
assert motors[0].stop() is None
assert motors[0].brake() is None
print('ready')
input()
for motor in motors:
    assert motor.dc(100) is None
print('ready')
input()
assert False, 'host Stop must coast all eight active PF outputs'
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await shell(port, captured, b"pup_peer color_distance")
                # Cached detection does not select a mode before the constructor.
                await port_matches(client, phase=7, type_id=37)
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await checked(client, Input(data=b"\n"))
                await ready(client)

                before = pf_history(await shell(port, captured, b"pup_peer pf"))
                # Hand-derived PF single-output messages: four low-bit commands,
                # then one full-power command for each channel/color address.
                assert [message for _, message in before] == [
                    0x042, 0x04e, 0x040, 0x048,
                    0x047, 0x059, 0x147, 0x157,
                    0x247, 0x257, 0x347, 0x357,
                ]
                spaced(before)
                mode, wire = output_frame(await shell(port, captured, b"pup_peer output"))
                assert mode == 7 and len(wire) == 7
                assert wire[:6] == bytes.fromhex("4600b9cf5703")
                checksum = 0
                for byte in wire[3:]:
                    checksum ^= byte
                assert checksum == 0xff

                await checked(client, Stop())
                result = await completed(client, state=3, timeout=6)
                assert result.cleanup == 0, result
                after = pf_history(await shell(port, captured, b"pup_peer pf"))
                assert after[:len(before)] == before
                assert len(after) == len(before) + 8
                assert sorted(message for _, message in after[len(before):]) == [
                    0x040, 0x050, 0x140, 0x150, 0x240, 0x250, 0x340, 0x350,
                ]
                spaced(after)
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0
            except BaseException as error:
                error.add_note(f"Console: {bytes(captured)!r}")
                for command in (b"pup_peer pf", b"observe port.A", b"observe motor.A"):
                    try:
                        async with asyncio.timeout(0.5):
                            error.add_note(repr(await shell(port, captured, command)))
                    except Exception:
                        break
                raise

    # PF exercises long asynchronous waits, not a sampled control loop. QEMU's
    # instruction clock makes this Python run loop CPU-bound on the host; use
    # the normal paced clock and check the transmitted frame timestamps.
    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_pf_motor_failed_cleanup_retries_only_coast():
    source = b"""
from pybricks.pupdevices import ColorDistanceSensor, PFMotor
from pybricks.parameters import Port, Color
import errno
motor = PFMotor(ColorDistanceSensor(Port.A), 1, Color.RED)
assert motor.dc(100) is None
print('ready'); input()
try:
    motor.dc(50)
except OSError as error:
    assert error.args[0] == errno.ENODEV
else:
    assert False
print('ready'); input()
assert False, 'host Stop must report an unavailable IR cleanup path'
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await shell(port, captured, b"pup_peer color_distance")
                first = await port_matches(client, phase=7, type_id=37)
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                assert [value for _, value in pf_history(await shell(port, captured, b"pup_peer pf"))] == [0x047]

                await shell(port, captured, b"pup_peer unplug")
                await port_matches(client, phase=1)
                await shell(port, captured, b"pup_peer color_distance")
                replacement = await port_matches(client, phase=7, type_id=37)
                assert replacement.values["generation"] != first.values["generation"]
                await checked(client, Input(data=b"\n"))
                await ready(client)
                assert pf_history(await shell(port, captured, b"pup_peer pf")) == []

                await shell(port, captured, b"pup_peer unplug")
                await port_matches(client, phase=1)
                assert not success(await client.request(Stop()))
                failed = await checked(client, Status())
                assert failed.state == 3 and failed.cleanup < 0, failed
                assert (await snapshot(client, "sensor.A")).values["owner"] == 1

                await shell(port, captured, b"pup_peer color_distance")
                await port_matches(client, phase=7, type_id=37)
                assert not success(await client.request(Stop()))  # Preserve the first cleanup error.
                assert [value for _, value in pf_history(await shell(port, captured, b"pup_peer pf"))] == [0x040]
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0
                assert (await checked(client, Status())).cleanup == failed.cleanup
                await checked(client, Stop())  # Finish teardown after the retained error is reported.
                assert (await checked(client, Status())).cleanup == failed.cleanup
            except BaseException as error:
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))
