# SPDX-License-Identifier: Apache-2.0
"""Protected Car calibration and drive commands with two synthetic UART motors."""

import asyncio

from test_native import native_console
from test_python import completed, output
from test_python_drivebase import prepare_motors
from test_python_hub import ready, shell
from test_python_imu import BUILD

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Interrupt, Start, Stop, checked, upload
from brickphyrdev.telemetry import snapshot


def test_car_calibration_drive_and_interrupted_cleanup():
    source = b"""
from pybricks.pupdevices import Motor
from pybricks.parameters import Port
from pybricks.robotics import Car

steer, drive = Motor(Port.A), Motor(Port.B)
print('ready'); input()
car = Car(steer, drive, torque_limit=50)
assert steer.angle() > 10  # The right end stop is beyond the steering margin.
print('ready'); input()
car.steer(100)
print('ready'); input()
car.steer(-100)
print('ready'); input()
car.drive_power(5)
print('ready'); input()
car.drive_power(50)
print('ready'); input()
car.drive_speed(-120)
print('ready'); input()

# A failing Car operation must not cancel a newer direct Motor command.
steer.dc(20)
drive.close()
try:
    car.drive_speed(120)
except OSError:
    print('ready'); input()
else:
    assert False, 'Car reused its closed drive Motor'
assert False, 'host Stop must release both motors'
"""
    interrupted = b"""
from pybricks.pupdevices import Motor
from pybricks.parameters import Port
from pybricks.robotics import Car
steer, drive = Motor(Port.A), Motor(Port.B)
print('ready'); input()
try:
    Car(steer, drive, torque_limit=50)
except KeyboardInterrupt:
    print('ready'); input()
else:
    assert False, 'host Interrupt must interrupt calibration'
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                async def observe(name, matches):
                    async with asyncio.timeout(3):
                        while True:
                            value = await snapshot(client, name)
                            if value.error == 0 and matches(value.values):
                                return value
                            await asyncio.sleep(0.005)

                async def released():
                    for name in ("motor.A", "motor.B"):
                        motor = await snapshot(client, name)
                        assert motor.error in (0, -11) and motor.values["owner"] == 0, motor
                        assert motor.values["mode"] == motor.values["duty_permille"] == 0, motor

                await prepare_motors(client, port, captured)
                # Start the encoder mid-turn so both synthetic endpoints stay
                # away from the encoder's wrap boundary.
                await shell(port, captured, b"pup_peer motor_angle 1800")
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)

                # The peer emits encoder motion independently of PWM. Travel
                # toward each endpoint, then freeze it to trigger PBIO stall.
                origin = (await snapshot(client, "sensor.A")).values["angle_mdeg"]
                await shell(port, captured, b"pup_peer motor_step -20")
                await checked(client, Input(data=b"\n"))
                await observe("sensor.A", lambda v: v["angle_mdeg"] <= origin - 20000)
                await shell(port, captured, b"pup_peer motor_step 0")
                await observe("control.A", lambda v: v["active"] and v["reference_mdps"] > 0)
                await shell(port, captured, b"pup_peer motor_step 20")
                await observe("sensor.A", lambda v: v["angle_mdeg"] >= origin + 20000)
                await shell(port, captured, b"pup_peer motor_step 0")
                await ready(client)
                center = await snapshot(client, "control.A")
                assert center.error == 0 and center.values["active"], center

                await checked(client, Input(data=b"\n"))
                await ready(client)
                right = await snapshot(client, "control.A")
                assert right.error == 0 and right.values["reference_mdeg"] > center.values["reference_mdeg"], right
                await checked(client, Input(data=b"\n"))
                await ready(client)
                left = await snapshot(client, "control.A")
                assert left.error == 0 and left.values["reference_mdeg"] < center.values["reference_mdeg"], left

                await checked(client, Input(data=b"\n"))
                await ready(client)
                drive = await snapshot(client, "motor.B")
                assert drive.error == 0 and drive.values["mode"] == drive.values["duty_permille"] == 0, drive
                await checked(client, Input(data=b"\n"))
                await ready(client)
                drive = await snapshot(client, "motor.B")
                assert drive.error == 0 and drive.values["mode"] == 1, drive
                assert 570 <= drive.values["duty_permille"] <= 630, drive
                await checked(client, Input(data=b"\n"))
                await ready(client)
                drive = await snapshot(client, "control.B")
                assert drive.error == 0 and drive.values["active"] and drive.values["reference_mdps"] < 0, drive

                await checked(client, Input(data=b"\n"))
                await ready(client)
                steer = await snapshot(client, "motor.A")
                assert steer.error == 0 and steer.values["owner"] == 1, steer
                assert steer.values["mode"] == 1 and steer.values["duty_permille"] > 0, steer
                drive = await snapshot(client, "motor.B")
                assert drive.values["owner"] == drive.values["mode"] == 0, drive

                await checked(client, Stop())
                result = await completed(client, state=3)
                assert result.cleanup == 0, result
                await released()

                await upload(client, port, interrupted, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await checked(client, Input(data=b"\n"))
                await observe("control.A", lambda v: v["active"] and v["reference_mdps"] < 0)
                await checked(client, Interrupt())
                await ready(client)
                for name in ("motor.A", "motor.B"):
                    motor = await snapshot(client, name)
                    assert motor.error == 0 and motor.values["owner"] == 1, motor
                    assert motor.values["mode"] == motor.values["duty_permille"] == 0, motor
                await checked(client, Input(data=b"\n"))
                result = await completed(client)
                assert result.result == result.fault == result.cleanup == 0, result
                await released()
            except BaseException as error:
                error.add_note(f"Python output: {await output(client)!r}")
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))
