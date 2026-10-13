# SPDX-License-Identifier: Apache-2.0
import errno
from pybricks.iodevices import LWP3Device
from pybricks.messaging import BLERadio
from pybricks.parameters import Button, Color
from pybricks.pupdevices import Remote
from pybricks.tools import run_task

radio = BLERadio(observe_channels=(7,))
try:
    Remote(name="Missing", timeout=0)
    raise AssertionError("scan must time out")
except OSError as error:
    assert error.args[0] == errno.ETIMEDOUT
print("searching")
remote = Remote()
assert remote.name() == "Remote"
print("remote ready")
input()
assert remote.buttons.pressed() == {Button.LEFT_PLUS, Button.RIGHT_MINUS, Button.CENTER}
remote.light.on(Color.RED)
remote.name("Renamed")
assert remote.name() == "Renamed"
remote.__del__()
# Reclaim the same live connection with a different view and fresh notification queue.
raw = LWP3Device(0x42, name="Rename", num_notifications=2)
assert raw.name() == "Renamed"
print("raw ready")
input()
assert raw.read() == b"\x05\x00\x08\x02\x02"
assert raw.read() == b"\x05\x00\x08\x02\x03"
assert raw.read() is None
raw.disconnect()


# Constructors are blocking-only, so construct before entering the run loop.
paired = LWP3Device(0x42, connect=False, pair=True)


async def paired_commands():
    await paired.connect()
    await paired.write(b"\x05\x00\x01\x02\x05")
    await paired.disconnect()


run_task(paired_commands())
print("paired done")
input()
remote = Remote()
print("stop ready")
input()
remote.light.on(Color.GREEN)
raise AssertionError("test must force-stop the pending write")
