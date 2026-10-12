# SPDX-License-Identifier: Apache-2.0
"""Protected Motor bindings over synthetic UART feedback and real motor services."""

import asyncio
import re

from test_native import native_console
from test_python import completed, output
from test_python_accessories import motor_matches
from test_python_hub import BUILD, ready, shell
from test_python_io import until
from test_python_pupdevice import port_matches

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Start, Stop, checked, upload
from brickphyrdev.telemetry import snapshot


def test_motor_coordinates_motion_and_cancellation():
    source = b"""
from pybricks.pupdevices import Motor
from pybricks.parameters import Port, Direction, Stop
from pybricks.tools import wait, run_task, multitask

m = Motor(Port.A, Direction.COUNTERCLOCKWISE, [[12, 36], [20, 40]], profile=6)
assert m.angle() == 0 and m.speed() == 0 and m.done()
assert m.load() == 0 and not m.stalled()
print('ready'); input()
wait(25)
assert m.angle() == -15, m.angle()

alias = Motor(Port.A, Direction.COUNTERCLOCKWISE, [10, 60], reset_angle=False)
assert alias.angle() == -15
assert alias.reset_angle(12) is None
assert m.angle() == 12, m.angle()
print('ready'); input()
wait(25)
assert m.angle() == 9, m.angle()
assert m.reset_angle() is None
assert alias.angle() == -18, alias.angle()
print('ready'); input()
wait(120)
assert -100 <= m.speed() <= -60, m.speed()
assert m.load() == 0
print('ready'); input()
wait(25)

m.reset_angle(0)
c = m.control
assert c is m.control and c.scale == 6000
assert c.trajectory() is None and c.done() and c.load() == 0 and not c.stalled()
limits, pid = c.limits(), c.pid()
target, stall = c.target_tolerances(), c.stall_tolerances()
assert c.limits(speed=-60, acceleration=(100, 150), torque=30) is None
assert alias.control.limits() == (60, (100, 150), 30)
m.run(1000)
t = c.trajectory()
assert len(t) == 13 and t[9] == 360 and t[11] == 600, t
# A live settings update must retain the trajectory and its request identity.
c.pid(kp=pid[0] + 1)
assert alias.control.pid()[0] == pid[0] + 1
assert c.trajectory() == t
c.limits(speed=40)
assert c.trajectory() == t
m.run(1000)
assert c.trajectory()[9] == 240, c.trajectory()
m.stop()
assert c.trajectory() is None and c.load() == 0
c.target_tolerances(speed=4, position=2)
c.stall_tolerances(speed=3, time=30)
assert alias.control.target_tolerances() == (4, 2)
assert alias.control.stall_tolerances() == (3, 30)
try:
    c.scale = 1
    assert False, 'scale must be read-only'
except AttributeError:
    pass
c.limits(*limits)
c.pid(*pid)
c.target_tolerances(*target)
c.stall_tolerances(*stall)
c.limits(speed=30, acceleration=100, torque=30)
m.reset_angle(0)
assert c.limits() == (30, 100, 30)
alias = Motor(Port.A, Direction.COUNTERCLOCKWISE, [10, 60], reset_angle=False)
assert c.limits() == limits and c.pid() == pid
assert m.run_target(0, 1000) is None and m.done()
assert m.angle() == 0
assert m.run_angle(0, -360) is None and m.done()
assert m.run_time(60, 0) is None and m.done()
assert m.run_time(60, -1, then=Stop.COAST) is None
assert m.run_time(60, 40, then=Stop.BRAKE) is None and m.done()
assert m.hold() is None
assert m.track_target(1) is None
print('ready'); input()

assert m.stop() is None
assert m.run_target(-60, -12, wait=False) is None
print('ready'); input()
assert m.stop() is None
assert m.run_angle(-60, -12, then=Stop.COAST_SMART, wait=False) is None
print('ready'); input()
m.stop()
assert m.run_until_stalled(200, duty_limit=50) == m.angle()
assert m.done()

async def timed():
    assert await m.run_time(50, 25, then=Stop.BRAKE) is None
    assert m.done()
    pending = m.run_time(80, 500, then=Stop.BRAKE)
    # A second object replaces that motion. Closing the old awaitable must
    # leave the newer command running, even though both share one port.
    alias.run_time(-60, 500, wait=False)
    pending.close()
    assert not alias.done()
    alias.stop()

async def slow():
    await m.run_time(100, 500)

async def replace():
    await wait(10)
    m.run_time(-60, 500, wait=False)

run_task(timed())
run_task(multitask(slow(), replace()))
assert not m.done()
assert m.reset_angle(25) is None
assert m.angle() == 25
# The interrupted timed motion uses hold on reset. User data and origin remain shared.
assert alias.angle() == 25
m.settings(1000)
m.run(-200)
print('ready'); input()
assert False, 'host Stop must release the active motor'
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(
            captured.extend, port=device, management=True
        ) as (port, client):
            try:
                # Wait only for electrical attachment/metadata. No observer or
                # trajectory is started before the Python constructor.
                await shell(port, captured, b"pup_peer encoded_motor")
                await port_matches(client, phase=6)
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await motor_matches(client, owner=1, mode=0, duty_permille=0)
                for command in (
                    b"pup_peer motor_angle 900",
                    b"pup_peer motor_angle 1080",
                    b"pup_peer motor_step 100",
                    b"pup_peer motor_step 0",
                ):
                    await shell(port, captured, command)
                    await checked(client, Input(data=b"\n"))
                    await ready(client)

                value = await snapshot(client, "control.A")
                assert value.error == 0 and value.values["active"], value
                # Last reference was an immediate one-degree output target,
                # corresponding to six degrees in the negative shaft direction.
                measured = await snapshot(client, "sensor.A")
                assert measured.error == 0, measured
                assert value.values["reference_mdeg"] == measured.values["angle_mdeg"] - 6000, value
                assert value.values["reference_mdps"] == 0, value
                await checked(client, Input(data=b"\n"))
                await ready(client)
                value = await snapshot(client, "control.A")
                assert value.error == 0 and value.values["active"], value
                assert value.values["reference_mdps"] > 0, value
                await checked(client, Input(data=b"\n"))
                await ready(client)
                value = await snapshot(client, "control.A")
                assert value.error == 0 and value.values["active"], value
                assert value.values["reference_mdps"] < 0, value
                await checked(client, Input(data=b"\n"))
                await ready(client)
                value = await motor_matches(client, owner=1, mode=2)
                assert 0 < value.values["duty_permille"] < 160, value
                await checked(client, Stop())
                result = await completed(client, state=3)
                assert result.cleanup == 0, result
                await motor_matches(client, owner=0, mode=0, duty_permille=0)
            except BaseException as error:
                error.add_note(f"Python output: {await output(client)!r}")
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))


def test_motor_model_and_log_lifetime():
    source = b"""
from pybricks.pupdevices import Motor
from pybricks.parameters import Port, Direction
from pybricks.tools import wait

m = Motor(Port.A, Direction.COUNTERCLOCKWISE, [12, 24])
alias = Motor(Port.A, Direction.COUNTERCLOCKWISE, [12, 24], reset_angle=False)
model = m.model
assert model is m.model and m.log is m.log
assert m.control.log is m.control.log and m.control.log is not m.log
settings = list(model.settings())
settings[4] += 1
assert model.settings(settings) is None
assert alias.model.settings() == tuple(settings)
m.reset_angle(15)
assert model.settings() == tuple(settings)
angle, speed, current, stalled = model.state()
# Observer state uses directed shaft units, without the 2:1 output gearing.
assert abs(angle - 30) < 0.1 and speed == current == 0 and not stalled

m.dc(20)
assert m.log.start(40, down_sample=2) is None
wait(60)
m.stop()
assert m.log.stop() is None
m.log.save('motor.csv')
m.log.start(20)
wait(25)
m.close()
# Closing the motor stops collection but retains already initialized rows.
m.log.save('closed.csv')

m = Motor(Port.A, Direction.COUNTERCLOCKWISE, [12, 24])
m.reset_angle(15)
m.control.log.start(40, down_sample=2)
wait(20)
m.control.log.save('idle-control.csv')
m.log.start(40, down_sample=2)
m.control.log.start(40, down_sample=2)
m.run_time(80, 60)
m.close()
m.log.save('servo.csv')
m.control.log.save('control.csv')

m = Motor(Port.A)
# Together these buffers occupy most of the kernel heap: leaking either
# prevents the next application's equally sized pair after forced cleanup.
m.log.start(2600)
m.control.log.start(2600)
print('ready'); input()
"""
    second = b"""
from pybricks.pupdevices import Motor
from pybricks.parameters import Port
m = Motor(Port.A)
m.log.start(2600)
m.control.log.start(2600)
print('log memory reclaimed')
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await shell(port, captured, b"pup_peer encoded_motor")
                await port_matches(client, phase=6)
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                text = await until(client, b"ready\r\n")
                logs = re.findall(rb"PB_OF:([^\r\n]+)\r\n(.*?)PB_EOF\r\n", text, re.S)
                assert [name for name, _ in logs] == [b"motor.csv", b"closed.csv", b"idle-control.csv", b"servo.csv", b"control.csv"], text
                for name, data in logs:
                    rows = [[int(v) for v in row.split(b", ")] for row in data.splitlines()]
                    if name == b"idle-control.csv":
                        assert rows == [], rows
                        continue
                    columns = 13 if name == b"control.csv" else 11
                    assert len(rows) == 4 and all(len(row) == columns for row in rows), rows
                    assert all(row[2] == 15 and row[3] == 0 for row in rows), rows
                    period = 5 if name == b"closed.csv" else 10
                    assert all(b[0] - a[0] == period and b[1] - a[1] == period * 10
                               for a, b in zip(rows, rows[1:])), rows
                    if name == b"motor.csv":
                        assert all(row[4] & 3 == 2 and row[5] > 0 for row in rows), rows
                        assert all(row[8] == row[9] == 0 for row in rows), rows
                    elif name == b"closed.csv":
                        assert all(row[4] & 3 == 0 and row[5] == 0 for row in rows), rows
                    elif name == b"control.csv":
                        # The fixed synthetic shaft remains at 15 output degrees;
                        # its positive command/torque must survive CCW+2:1 conversion.
                        assert rows[-1][6] > 15 and all(row[7] > 0 for row in rows), rows
                        assert all(row[5] > 0 and row[10] > 0 and row[12] > 0 for row in rows), rows
                await checked(client, Stop())
                await completed(client, state=3)
                await motor_matches(client, owner=0, mode=0, duty_permille=0)
                await upload(client, port, second, Format.PYTHON)
                await checked(client, Start())
                state = await completed(client)
                assert state.result == state.fault == state.dropped == 0, state
                assert await output(client) == b"log memory reclaimed\r\n"
            except BaseException as error:
                error.add_note(f"Python output: {await output(client)!r}")
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))
