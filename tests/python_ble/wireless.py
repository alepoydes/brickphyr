# SPDX-License-Identifier: Apache-2.0
# The fixture supplies synthetic LEGO peripherals; no motors are operated.
import errno
from pybricks.parameters import Color
from pybricks.pupdevices import TechnicMoveHub, MarioHub, DuploTrain
from pybricks.tools import run_task, wait
from pybricks.hubs import PrimeHub
from pybricks import iodevices, pupdevices, tools

# This profile has radio peers but no local peripherals. A usable hub and
# wireless classes must not pull in disabled wired or hub-device bindings.
hub = PrimeHub()
assert hub.system.name()
for name in ('light', 'bluetooth_light', 'display', 'battery', 'charger', 'speaker', 'imu'):
    assert not hasattr(hub, name)
assert hasattr(hub, 'ble')
assert not hasattr(tools, 'hub_menu')
for name in ('PUPDevice', 'LUMPDevice', 'UARTDevice', 'DCMotor'):
    assert not hasattr(iodevices, name)
for name in ('Motor', 'DCMotor', 'ColorSensor', 'Light', 'ColorLightMatrix', 'PFMotor'):
    assert not hasattr(pupdevices, name)
try:
    import pybricks.robotics
    raise AssertionError('motor control is disabled')
except ImportError:
    pass


def missing(read):
    try:
        read()
        raise AssertionError("no sample has arrived")
    except OSError as error:
        assert error.args[0] == errno.EAGAIN


move = TechnicMoveHub()
print("move ready")
input()
try:
    move.drive(150, -150)
    raise AssertionError("ATT failure must reach the application")
except OSError as error:
    assert error.args[0] == errno.EIO
move.drive(150, -150)
move.drive(150, -150)
move.drive(150, -150)  # Two acknowledged repeats suffice inside 500 ms.
print("move sent")
input()


async def move_again():
    await wait(500)
    await move.drive(150, -150)
    await move.drive(-37, 14)
    await move.disconnect()


run_task(move_again())
print("move done")
input()
mario = MarioHub(connect=False)
mario.detectable_colors((Color.MAGENTA, Color.RED))
mario.connect()
missing(mario.color)
print("mario ready")
input()
assert mario.color() == Color.RED
assert mario.hsv().h == 10
mario.detectable_colors(())
assert mario.color() is None
mario.detectable_colors((Color.BLUE,))
assert mario.color() == Color.BLUE
mario.disconnect()
assert mario.detectable_colors() == (Color.BLUE,)
print("mario disconnected")
input()
mario.connect()
missing(mario.hsv)  # A new physical link cannot retain the previous sample.
print("mario again")
input()
assert mario.hsv().s > 90 and mario.hsv().v > 90
assert mario.color() == Color.BLUE
mario.disconnect()
print("mario done")
input()

train = DuploTrain()
print("old ready")
input()
missing(train.speed)
missing(train.color)  # Other train generation's ports must not fill snapshots.
print("old empty")
input()
assert train.speed() == -123 and train.color() == Color.RED
train.drive(power=-150)
train.headlights(Color.RED)
train.sound("horn")
try:
    train.sound("unknown")
    raise AssertionError("unknown sound must not send a packet")
except ValueError:
    pass
print("old sent")
input()
assert train.color() == Color.WHITE
train.disconnect()
print("old done")
input()

train = DuploTrain()
print("blue ready")
input()
assert train.speed() == 42 and train.color() == Color.VIOLET
train.drive(125)
train.headlights(Color.BLUE)
train.sound("water")
train.sound("horn")  # Reference second-generation firmware has one packet.
print("blue sent")
input()
assert train.color() == Color.NONE
train.disconnect()
print("wireless done")
