# SPDX-License-Identifier: Apache-2.0
# Synthetic BLE HID reports; no physical controller or motor is used.
import errno
from pybricks.iodevices import XboxController
from pybricks.parameters import Button
from pybricks.tools import run_task


def fails(read, code):
    try:
        read()
        raise AssertionError('expected missing or invalid report')
    except OSError as error:
        assert error.args[0] == code


controller = XboxController(name='Xbox Wireless')
assert controller.name() == 'Xbox Wireless Controller'
fails(controller.joystick_left, errno.EAGAIN)
print('xbox ready')
input()
assert controller.joystick_left() == (100, 100)
assert controller.joystick_right() == (0, 0)
assert controller.triggers() == (50, 100)
assert controller.dpad() == 2
assert controller.buttons.pressed() == {
    Button.A, Button.X, Button.LB, Button.VIEW, Button.GUIDE,
    Button.UPLOAD, Button.UP, Button.RIGHT,
}
fails(controller.profile, errno.ENOTSUP)
assert controller.state() == (32768, -32767, 1, 1, 512, 1023, 2, 0x1449, 1, 0, 0, 0)
controller.rumble(power=0)
controller.rumble(count=-1)
print('xbox standard')
input()
fails(controller.buttons.pressed, errno.EPROTO)
print('xbox rejected')
input()
assert controller.joystick_left() == (0, 0)
assert controller.joystick_right() == (0, 0)
assert controller.triggers() == (0, 0)
assert controller.dpad() == 8 and controller.profile() == 2
assert controller.buttons.pressed() == {
    Button.B, Button.MENU, Button.P1, Button.P3, Button.UP, Button.LEFT,
}
assert controller.state()[-3:] == (2, 0x21, 5)


async def feedback():
    await controller.rumble(power=(150, -2, 20, 30), duration=3000, count=3)
    await controller.rumble(duration=1, count=1)
    await controller.disconnect()


run_task(feedback())
print('xbox disconnected')
input()
# Main stops the interpreter while the reconnection's descriptor read is held.
controller.connect()
raise AssertionError('held read completed unexpectedly')
