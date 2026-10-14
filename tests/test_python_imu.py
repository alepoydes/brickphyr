# SPDX-License-Identifier: Apache-2.0
"""IMU binding checks; estimator math and acquisition belong to the C fixtures."""

import asyncio
from pathlib import Path

from test_native import native_console
from test_python import completed, output
from test_python_hub import shell
from test_python_io import until
from test_python_modules import execute

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Repl, Start, Stop, checked, upload

BUILD = Path(__file__).resolve().parents[1] / "build/python-imu-tests"
IMPORTS = b"""
from pybricks.hubs import PrimeHub
from pybricks.parameters import Axis, Side
from pybricks.tools import StopWatch, wait
imu = PrimeHub().imu
"""


async def run(client, port, source):
    await upload(client, port, IMPORTS + source, Format.PYTHON)
    await checked(client, Start())
    state = await completed(client)
    text = await output(client)
    assert state.result == state.fault == state.cleanup == state.dropped == 0, (state, text)
    assert text == b"passed\r\n", text


def test_imu_binding_and_application_state():
    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await run(client, port, b"""
# One non-default frame checks binding units/mapping. Six poses and winding
# are covered directly by the production estimator's numerical checks.
assert PrimeHub(top_side=Axis.X * 2, front_side=Axis.Y * 3).imu is imu
for calibrated in (False, True):
    assert imu.up(calibrated) == Side.TOP
    assert abs(imu.acceleration(-Axis.X, calibrated) + 9804.35) < 1
    a = imu.acceleration(calibrated=calibrated)
    assert a.shape == (3, 1) and abs(a[0, 0] - 9804.35) < 1
    assert abs(imu.tilt(calibrated)[0] + 90) < 0.01
assert abs(imu.orientation()[2, 2] - 1) < 0.0001
imu.reset_heading(37)
assert abs(imu.heading() - 37) < 0.001

# A mixed valid/invalid update must remain atomic across the Python boundary.
original = imu.settings()
try:
    imu.settings(angular_velocity_threshold=7, angular_velocity_scale=(360, 1, 360))
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
print('passed')
""")
                identity = b"base: 1000 0 0 0 1000 0 0 0 1000 pending:0 error:0"
                assert identity in await shell(port, captured, b"imu_view")
                # The shared terminal sees the same selected values and its
                # physical-axis ordering, including after application exit.
                reply = await shell(port, captured, b"config get imu/accel_calibration_um_s2")
                assert b"9800000 9802000 9804000 -9801000 -9803000 -9805000" in reply, reply

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

                await shell(port, captured, b"imu_input 0 0 -1000 0 0 -4096")
                await run(client, port, b"""
assert imu.settings()[1] == 1234.5
old_epoch = imu.epoch()
imu.settings(angular_velocity_bias=(0, 0, 0), angular_velocity_scale=(360, 360, 350),
             heading_correction=360, acceleration_correction=(9806.65, -9806.65) * 3)
wait(10)
assert imu.epoch() != old_epoch
assert abs(imu.angular_velocity(Axis.Z, False) - 70) < 0.01
assert abs(imu.angular_velocity(Axis.Z) - 72) < 0.01
imu.reset_heading(123)
r = imu.rotation(Axis.Z)
u = imu.rotation(Axis.Z, False)
epoch = imu.epoch()
timer = StopWatch()
# 100 ms gives about 80 samples and 7 degrees of rotation, sufficient to
# distinguish the two scales without waiting for calibration or a full turn.
wait(100)
elapsed = timer.time() / 1000
assert imu.epoch() == epoch
assert abs(imu.rotation(Axis.Z) - r - 72 * elapsed) < 0.3
assert abs(imu.rotation(Axis.Z, False) - u - 70 * elapsed) < 0.3
assert abs(imu.heading() - 123 + 72 * elapsed) < 0.3
print('passed')
""")
                # Configuration completion must not depend on another ready
                # edge. The C fixture covers the acquisition errors themselves.
                await shell(port, captured, b"imu_error 2")
                await run(client, port, b"""
wait(10)
assert len(imu.settings()) == 6
try:
    imu.reset_heading(0)
except OSError as ex:
    assert ex.args[0] == 116
else:
    assert False
print('passed')
""")
            finally:
                await checked(client, Stop())

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))


def test_imu_disabled_keeps_other_hub_functions_usable():
    assert execute(b"""
from pybricks.hubs import PrimeHub
from pybricks.parameters import Axis
from pybricks import iodevices, pupdevices, messaging
hub = PrimeHub()
assert hub.system.name() and isinstance(hub.buttons.pressed(), set)
assert not hasattr(hub, 'imu') and not hasattr(hub, 'ble')
assert not hasattr(messaging, 'BLERadio') and not hasattr(messaging, 'HubNetwork')
assert not hasattr(iodevices, 'LWP3Device') and not hasattr(pupdevices, 'Remote')
assert iodevices.PUPDevice and pupdevices.Motor
for operation in (lambda: PrimeHub(top_side=Axis.X, front_side=Axis.Y),
                  lambda: PrimeHub(observe_channels=[1])):
    try:
        operation()
    except OSError as ex:
        assert ex.args[0] == 134
    else:
        assert False
print('passed')
""") == b"passed\n"
