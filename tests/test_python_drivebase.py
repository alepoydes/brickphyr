# SPDX-License-Identifier: Apache-2.0
"""Protected robotics bindings with two synthetic UART motors."""

import asyncio

from test_native import native_console
from test_python import completed, output
from test_python_hub import ready, shell
from test_python_imu import BUILD
from test_python_pupdevice import port_matches

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Interrupt, Start, Stop, checked, upload
from brickphyrdev.telemetry import snapshot


async def prepare_motors(client, port, captured):
    # The Python constructors establish both observers from cold
    # state; only electrical peers and metadata exist beforehand.
    await shell(port, captured, b"pup_peer encoded_motor")
    await shell(port, captured, b"pup_peer B encoded_motor")
    await port_matches(client, phase=6)
    async with asyncio.timeout(3):
        while True:
            peer_b = await snapshot(client, "port.B")
            if peer_b.values["phase"] == 6:
                break
            await asyncio.sleep(0.005)

    # Battery acquisition deliberately exposes the one-second
    # power-on settling interval. Observation does not start it.
    async with asyncio.timeout(3):
        while (await snapshot(client, "battery")).error == -119:
            await asyncio.sleep(0.005)


def test_drivebase_pair_cancellation_reuse_and_cleanup():
    source = b"""
from pybricks.pupdevices import Motor
from pybricks.parameters import Port
from pybricks.robotics import DriveBase
from pybricks.hubs import PrimeHub
from pybricks.tools import run_task, wait

hub = PrimeHub()
left = Motor(Port.A)
right = Motor(Port.B)
base = DriveBase(left, right, wheel_diameter=56, axle_track=112)
assert base.state() == (0, 0, 0, 0) and base.done()
assert base.use_gyro(True) is None
assert base.reset(12, -7) is None
assert base.distance() == 12 and base.angle() == -7
assert base.state() == (12, 0, -7, 0)
assert abs(hub.imu.heading() + 7) < 0.01
settings = base.settings()
assert len(settings) == 4
assert base.settings(straight_speed=100, turn_rate=45) is None
assert abs(base.settings()[0] - 100) <= 1
assert abs(base.settings()[2] - 45) <= 1
assert base.reset() is None and base.state() == (0, 0, 0, 0)
assert base.straight(0) is None and base.done()
assert base.turn(0) is None and base.done()
assert base.move_by(0, 0) is None and base.done()

async def zero_move():
    assert await base.straight(0) is None
    assert await base.move_by(0, 0) is None
    assert base.done()

run_task(zero_move())
assert base.drive(80, 0) is None
assert base.use_gyro(True) is None
try:
    hub.imu.reset_heading(0)
    assert False, 'active gyro control must retain its heading reference'
except OSError:
    pass
wait(25)
assert base.done()  # Pybricks' endless drive is non-waiting, even while active.
print('ready'); input()

assert left.dc(20) is None
assert base.done()
print('ready'); input()

assert base.stop() is None
assert base.use_gyro(False) is None
assert base.brake() is None
assert base.hold() is None
assert left.stop() is None

async def replace():
    pending = base.move_by(100, 100)
    try:
        base.straight(2147483647)
    except ValueError:
        pass
    else:
        assert False, 'out-of-range replacement was accepted'
    assert next(pending) is None  # Rejection must retain the prior awaitable.
    assert base.drive(-80, 0) is None
    # Closing the superseded awaitable must leave the newer drive running.
    pending.close()
    assert base.done()

run_task(replace())
wait(25)
assert base.done()
print('ready'); input()
assert False, 'host Stop must release both paired motors'
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(
            captured.extend, port=device, management=True
        ) as (port, client):
            try:
                await prepare_motors(client, port, captured)

                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                for name in ("motor.A", "motor.B"):
                    motor = await snapshot(client, name)
                    assert motor.error == 0 and motor.values["owner"] == 1, motor
                    assert motor.values["mode"] == 1 and motor.values["duty_permille"] > 0, motor

                await checked(client, Input(data=b"\n"))
                await ready(client)
                left_state = await snapshot(client, "motor.A")
                right_state = await snapshot(client, "motor.B")
                assert left_state.error == right_state.error == 0, (left_state, right_state)
                assert left_state.values["owner"] == right_state.values["owner"] == 1
                assert left_state.values["mode"] == 1 and left_state.values["duty_permille"] > 0, left_state
                assert right_state.values["mode"] == right_state.values["duty_permille"] == 0, right_state

                await checked(client, Input(data=b"\n"))
                await ready(client)
                for name in ("motor.A", "motor.B"):
                    motor = await snapshot(client, name)
                    assert motor.error == 0 and motor.values["owner"] == 1, motor
                    assert motor.values["mode"] == 1 and motor.values["duty_permille"] < 0, motor

                await checked(client, Stop())
                result = await completed(client, state=3)
                assert result.cleanup == 0, result
                for name in ("motor.A", "motor.B"):
                    motor = await snapshot(client, name)
                    # Release restores the default measurement; its first
                    # encoder sample may still be pending (EAGAIN).
                    assert motor.error in (0, -11) and motor.values["owner"] == 0, motor
                    assert motor.values["valid"] and motor.values["failures"] == 0, motor
                    assert motor.values["mode"] == motor.values["duty_permille"] == 0, motor
            except BaseException as error:
                error.add_note(f"Python output: {await output(client)!r}")
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))


def test_spikebase_wheel_units_waits_and_steering():
    source = b"""
from pybricks.pupdevices import Motor
from pybricks.parameters import Port, Stop
from pybricks.robotics import SpikeBase
from pybricks.tools import StopWatch, run_task, wait

left = Motor(Port.A)
right = Motor(Port.B)
base = SpikeBase(left, right)
assert base.left is left and base.right is right
assert base.tank_move_for_degrees(0, 0, 123) is None
assert base.steering_move_for_degrees(100, 100, -20, wait=False) is None
wait(25)
print('ready'); input()
base.stop()

# SpikeBase retains blocking waits, also inside run_task.
async def timed():
    watch = StopWatch()
    assert base.tank_move_for_time(120, 120, 25, then=Stop.COAST) is None
    assert watch.time() >= 25
    assert base.steering_move_for_time(120, -100, 25, then=Stop.BRAKE) is None
run_task(timed())
print('ready'); input()

base.tank_move_forever(120, 120)
try:
    base.steering_move_forever(120, 101)
except ValueError:
    pass
else:
    assert False, 'invalid steering replaced a running pair'
wait(25)
print('ready'); input()

base.stop()
base.steering_move_forever(120, 100)
wait(25)
print('ready'); input()

try:
    base.tank_move_for_time(120, 120, 10000)
except KeyboardInterrupt:
    print('ready'); input()
else:
    assert False, 'host must interrupt the blocking SpikeBase wait'
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(
            captured.extend, port=device, management=True
        ) as (port, client):
            try:
                await prepare_motors(client, port, captured)
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                for name in ("motor.A", "motor.B"):
                    value = await snapshot(client, name)
                    assert value.error == 0 and value.values["duty_permille"] > 0, value

                await checked(client, Input(data=b"\n"))
                await ready(client)
                for name in ("motor.A", "motor.B"):
                    value = await snapshot(client, name)
                    assert value.error == 0 and value.values["mode"] == 1, value
                    assert value.values["duty_permille"] == 0, value

                for signs in ((-1, 1), (-1, -1)):
                    await checked(client, Input(data=b"\n"))
                    await ready(client)
                    for name, sign in zip(("control.A", "control.B"), signs):
                        value = await snapshot(client, name)
                        assert value.error == 0 and value.values["active"], value
                        assert value.values["reference_mdps"] * sign > 0, value

                await checked(client, Input(data=b"\n"))
                async with asyncio.timeout(3):
                    while (await snapshot(client, "control.B")).values["reference_mdps"] <= 0:
                        await asyncio.sleep(0.005)
                await checked(client, Interrupt())
                await ready(client)
                for name in ("motor.A", "motor.B"):
                    value = await snapshot(client, name)
                    assert value.error == 0 and value.values["owner"] == 1, value
                    assert value.values["mode"] == value.values["duty_permille"] == 0, value

                await checked(client, Input(data=b"\n"))
                result = await completed(client)
                assert result.result == result.fault == result.cleanup == 0, result
                for name in ("motor.A", "motor.B"):
                    value = await snapshot(client, name)
                    assert value.error in (0, -11) and value.values["owner"] == 0, value
                    assert value.values["valid"] and value.values["failures"] == 0, value
                    assert value.values["mode"] == value.values["duty_permille"] == 0, value
            except BaseException as error:
                error.add_note(f"Python output: {await output(client)!r}")
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))
