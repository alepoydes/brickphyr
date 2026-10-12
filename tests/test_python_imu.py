# SPDX-License-Identifier: Apache-2.0
"""Python IMU observations through protected ARM, acquisition and synthetic I2C."""

import asyncio
from pathlib import Path

from test_native import native_console
from test_python import completed, output
from test_python_hub import ready, shell
from test_python_io import until
from test_python_modules import execute

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Repl, Start, Stop, checked, upload
from brickphyrdev.telemetry import snapshot

BUILD = Path(__file__).resolve().parents[1] / "build/python-imu-tests"
IMPORTS = b"""
from pybricks.hubs import PrimeHub
from pybricks.parameters import Axis, Side
from pybricks.tools import Matrix, StopWatch, wait
imu = PrimeHub().imu
"""


async def run(client, port, source, *, timeout=3):
    await upload(client, port, IMPORTS + source, Format.PYTHON)
    await checked(client, Start())
    state = await completed(client, timeout=timeout)
    text = await output(client)
    assert state.result == state.fault == state.cleanup == state.dropped == 0, (state, text)
    assert text == b"passed\r\n", text


async def measured(client, error):
    async with asyncio.timeout(3):
        while (value := await snapshot(client, "imu.calibrated")).error != error:
            await asyncio.sleep(0.01)
    return value


def test_imu_vectors_pose_and_failures():
    # Independent six axis-aligned poses: wire registers, physical hub side,
    # gravity direction and expected pitch/roll. No copy of the estimator.
    poses = (
        ((-4096, 0, 0), "FRONT", (1, 0, 0), (-90, 0)),
        ((4096, 0, 0), "BACK", (-1, 0, 0), (90, 0)),
        ((0, 4096, 0), "LEFT", (0, 1, 0), (0, 90)),
        ((0, -4096, 0), "RIGHT", (0, -1, 0), (0, -90)),
        ((0, 0, -4096), "TOP", (0, 0, 1), (0, 0)),
        ((0, 0, 4096), "BOTTOM", (0, 0, -1), (0, 180)),
    )

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                for raw, side, direction, tilt in poses:
                    await shell(port, captured, b"imu_error 1")
                    await measured(client, -5)
                    await shell(port, captured, f"imu_input 0 0 0 {raw[0]} {raw[1]} {raw[2]}".encode())
                    await shell(port, captured, b"imu_error 0")
                    await measured(client, 0)
                    await run(client, port, f"""
direction = {direction}
expected_tilt = {tilt}
for calibrated in (False, True):
    assert imu.up(calibrated) == Side.{side}
    a = imu.acceleration(calibrated=calibrated)
    assert a.shape == (3, 1)
    for i, axis in enumerate((Axis.X, Axis.Y, Axis.Z)):
        assert abs(a[i, 0] - direction[i] * 9804.35) < 1
        assert abs(imu.acceleration(axis * 3, calibrated) - a[i, 0]) < 1
        assert abs(imu.acceleration(-axis, calibrated) + a[i, 0]) < 1
    pitch, roll = imu.tilt(calibrated)
    assert abs(pitch - expected_tilt[0]) < 0.01
    # At +/-90 pitch, roll is undefined. At 180 roll both signs are equivalent.
    if abs(pitch) < 89:
        assert abs(abs(roll) - abs(expected_tilt[1])) < 0.01
        if abs(roll) < 179:
            assert abs(roll - expected_tilt[1]) < 0.01
r = imu.orientation()
assert r.shape == (3, 3)
for i in range(3):
    assert abs(r[2, i] - direction[i]) < 0.0001
    for j in range(3):
        assert abs(sum(r[k, i]*r[k, j] for k in range(3)) - (i == j)) < 0.0001
print('passed')
""".encode())

                await run(client, port, b"""
for method in (imu.acceleration, imu.angular_velocity):
    for axis in (Matrix([[0], [0], [0]]), Matrix([[1, 0]]),
                 Axis.X * float('nan'), Axis.Z * float('inf')):
        try:
            method(axis)
        except ValueError:
            pass
        else:
            assert False
    try:
        method((1, 0, 0))
    except TypeError:
        pass
    else:
        assert False
assert abs(imu.acceleration(Matrix([[0, 0, -2]])) - 9804.35) < 1
print('passed')
""")
                for fault, error in ((1, 5), (2, 116)):
                    await shell(port, captured, b"imu_error 0")
                    await measured(client, 0)
                    await shell(port, captured, f"imu_error {fault}".encode())
                    await measured(client, -error)
                    await run(client, port, f"""
assert not imu.ready() and not imu.stationary()
for method in (imu.acceleration, imu.angular_velocity, imu.tilt, imu.up,
               imu.orientation, lambda: imu.acceleration(calibrated=False),
               lambda: imu.angular_velocity(calibrated=False)):
    try:
        method()
    except OSError as ex:
        assert ex.args[0] == {error}
    else:
        assert False
print('passed')
""".encode())
                await shell(port, captured, b"imu_error 0")
                await measured(client, 0)
                await upload(client, port, IMPORTS + b"print('ready')\nwhile True: imu.acceleration()\n", Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await checked(client, Stop())
                await run(client, port, b"assert imu.up() == Side.BOTTOM\nprint('passed')\n")
            finally:
                await checked(client, Stop())

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))


def test_imu_stationary_bias_and_rate_units():
    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            await shell(port, captured, b"imu_input 100 -200 300 0 0 -4096")
            await measured(client, 0)
            await run(client, port, b"""
raw = imu.angular_velocity(calibrated=False)
assert raw.shape == (3, 1)
for i, axis in enumerate((Axis.X, Axis.Y, Axis.Z)):
    assert abs(raw[i, 0] - (-7, -14, -21)[i]) < 0.01
    assert abs(imu.angular_velocity(axis, False) - raw[i, 0]) < 0.01
timer = StopWatch()
while not imu.ready():
    assert timer.time() < 3000
    wait(10)
assert imu.stationary()
for axis in (Axis.X, Axis.Y, Axis.Z):
    assert abs(imu.angular_velocity(axis)) < 0.01
print('passed')
""", timeout=6)

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))


def test_imu_disabled_keeps_other_hub_functions_usable():
    assert execute(IMPORTS + b"""
assert PrimeHub().system.name()
for method in (imu.ready, imu.stationary, imu.acceleration, imu.angular_velocity,
               imu.tilt, imu.up, imu.orientation, imu.heading, imu.epoch, imu.settings,
               lambda: imu.reset_heading(0), lambda: imu.rotation(Axis.X),
               lambda: PrimeHub(top_side=Axis.X, front_side=Axis.Y)):
    try:
        method()
    except OSError as ex:
        assert ex.args[0] == 134
    else:
        assert False
print('passed')
""") == b"passed\n"


def test_imu_frames_heading_calibration_and_cleanup():
    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await run(client, port, b"""
assert len(imu.settings()) == 6
for top, front, expected, tilt in ((Axis.X, Axis.Y, (1, 0, 0), (-90, 0)),
                                   (-Axis.Y, Axis.X, (0, -1, 0), (0, -90)),
                                   (-Axis.Z, -Axis.X, (0, 0, -1), (0, 180))):
    other = PrimeHub(top * 2, front * 3).imu
    assert other is imu
    assert imu.up() == Side.TOP and imu.up(False) == Side.TOP
    for calibrated in (False, True):
        a = imu.acceleration(calibrated=calibrated)
        for i in range(3):
            assert abs(a[i, 0] - 9804.35 * expected[i]) < 1
        pitch, roll = imu.tilt(calibrated)
        assert abs(pitch - tilt[0]) < 0.01
        if abs(pitch) < 89:
            assert abs(roll - tilt[1]) < 0.01
    assert abs(imu.orientation()[2, 2] - 1) < 0.0001
    imu.reset_heading(123)
    assert abs(imu.heading() - 123) < 0.001
    imu.reset_heading(1e30)
    imu.reset_heading(-17)
    assert abs(imu.heading() + 17) < 0.001
    assert abs(imu.rotation(Axis.X)) < 0.001
before = imu.acceleration()
for top, front in ((Axis.Z, Axis.Z), (Axis.Z * 0, Axis.X),
                   (Axis.Z * float('nan'), Axis.X), (Axis.Z, Axis.X * float('inf'))):
    try:
        PrimeHub(top_side=top, front_side=front)
    except ValueError:
        pass
    else:
        assert False
for angle in (float('nan'), float('inf')):
    try:
        imu.reset_heading(angle)
    except ValueError:
        pass
    else:
        assert False
assert abs(imu.heading() + 17) < 0.001
assert abs(imu.acceleration()[2, 0] - before[2, 0]) < 0.001
for axis in (None, (1, 0, 0)):
    try:
        imu.rotation(axis)
    except TypeError:
        pass
    else:
        assert False
print('passed')
""")
                identity = b"base: 1000 0 0 0 1000 0 0 0 1000 pending:0 error:0"
                assert identity in await shell(port, captured, b"imu_view")
                # A live application changes only its view. Physical telemetry
                # keeps hub Z upwards; forced stop restores default coordinates.
                await upload(client, port, IMPORTS + b"""
PrimeHub(top_side=Axis.X, front_side=Axis.Y)
imu.reset_heading(42)
print('ready')
while True: wait(10)
""", Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                physical = await snapshot(client, "imu.calibrated")
                assert physical.values["az_um_s2"] > 9_000_000, physical
                assert identity not in await shell(port, captured, b"imu_view")
                await checked(client, Stop())
                assert identity in await shell(port, captured, b"imu_view")

                await checked(client, Repl())
                await until(client, b">>> ")
                await checked(client, Input(data=b"from pybricks.hubs import PrimeHub; "
                    b"from pybricks.parameters import Axis; PrimeHub(Axis.X, Axis.Y)\r"))
                assert b"Traceback" not in await until(client, b">>> ")
                assert identity not in await shell(port, captured, b"imu_view")
                await checked(client, Input(data=b"\x04"))
                assert b"MPY: soft reboot" in await until(client, b">>> ")
                assert identity in await shell(port, captured, b"imu_view")
                await checked(client, Stop())

                await run(client, port, b"""
original = imu.settings()
for kwargs in ({'angular_velocity_threshold': -1},
               {'angular_velocity_bias': (0, 0)},
               {'angular_velocity_bias': (0, 0, 1e9)},
               {'angular_velocity_scale': (360, 0, 360)},
               {'acceleration_correction': (9806, 9806, 9806, -9806, 9806, -9806)},
               {'heading_correction': 1}, {'acceleration_threshold': float('nan')},
               {'angular_velocity_threshold': 7, 'angular_velocity_scale': (360, 1, 360)}):
    try:
        imu.settings(**kwargs)
    except ValueError:
        pass
    else:
        assert False
    assert imu.settings() == original
imu.settings(angular_velocity_threshold=0, acceleration_threshold=1234.5,
             angular_velocity_bias=(0.25, -0.5, 0.75),
             angular_velocity_scale=(355, 360, 365), heading_correction=361,
             acceleration_correction=(9800, -9801, 9802, -9803, 9804, -9805))
assert imu.settings() == (0, 1234.5, (0.25, -0.5, 0.75), (355, 360, 365),
                          (9800, -9801, 9802, -9803, 9804, -9805), 361)
wait(10)
assert abs(imu.angular_velocity(Axis.X) + 0.25 * 360 / 355) < 0.002
assert abs(imu.angular_velocity(Axis.Y) - 0.5) < 0.002
print('passed')
""")
                # Same settings are visible through the existing terminal; no
                # private Python calibration or persistence path exists.
                reply = await shell(port, captured, b"config get imu/accel_calibration_um_s2")
                assert b"9800000 9802000 9804000 -9801000 -9803000 -9805000" in reply, reply
                await run(client, port, b"""
assert imu.settings()[1] == 1234.5
imu.settings(angular_velocity_threshold=0, angular_velocity_bias=(0, 0, 0),
             angular_velocity_scale=(360, 360, 360), heading_correction=360,
             acceleration_correction=(9806.65, -9806.65) * 3)
wait(10)
print('passed')
""")
                await shell(port, captured, b"imu_input 0 0 -1000 0 0 -4096")
                await measured(client, 0)
                await run(client, port, b"""
assert abs(imu.angular_velocity(Axis.Z) - 70) < 0.01
imu.reset_heading(123)
r = imu.rotation(Axis.Z)
u = imu.rotation(Axis.Z, calibrated=False)
epoch = imu.epoch()
timer = StopWatch()
wait(300)
elapsed = timer.time() / 1000
assert imu.epoch() == epoch
assert abs(imu.rotation(Axis.Z) - r - 70 * elapsed) < 1
assert abs(imu.rotation(Axis.Z, False) - u - 70 * elapsed) < 1
assert abs(imu.heading() - 123 + 70 * elapsed) < 1
assert abs(imu.rotation(-Axis.Z) + imu.rotation(Axis.Z)) < 0.2
imu.settings(angular_velocity_scale=(360, 360, 350))
wait(10)
assert imu.epoch() != epoch
assert abs(imu.angular_velocity(Axis.Z) - 72) < 0.01
r = imu.rotation(Axis.Z)
u = imu.rotation(Axis.Z, False)
timer.reset()
wait(300)
elapsed = timer.time() / 1000
assert abs(imu.rotation(Axis.Z) - r - 72 * elapsed) < 1
assert abs(imu.rotation(Axis.Z, False) - u - 70 * elapsed) < 1
print('passed')
""")
                # A stopped ready signal cannot leave a configuration waiter
                # depending on another sample; errors remain observable.
                await shell(port, captured, b"imu_error 2")
                await measured(client, -116)
                await run(client, port, b"""
PrimeHub(top_side=Axis.X, front_side=Axis.Y)
assert len(imu.settings()) == 6
for method in (imu.heading, imu.epoch, lambda: imu.rotation(Axis.X),
               lambda: imu.reset_heading(0)):
    try:
        method()
    except OSError as ex:
        assert ex.args[0] == 116
    else:
        assert False
print('passed')
""")
                await shell(port, captured, b"imu_error 0")
                await measured(client, 0)
                await run(client, port, b"print('passed')\n")
            finally:
                await checked(client, Stop())

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))
