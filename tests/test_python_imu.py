# SPDX-License-Identifier: Apache-2.0
"""Python IMU observations through protected ARM, acquisition and synthetic I2C."""

import asyncio
from pathlib import Path

from test_native import native_console
from test_python import completed, output
from test_python_hub import ready, shell
from test_python_modules import execute

from brickphyrdev import console
from brickphyrdev.application import Format, Start, Stop, checked, upload
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
               imu.tilt, imu.up, imu.orientation):
    try:
        method()
    except OSError as ex:
        assert ex.args[0] == 134
    else:
        assert False
print('passed')
""") == b"passed\n"
