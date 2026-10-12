# SPDX-License-Identifier: Apache-2.0
"""Generic device I/O through ARM user mode and the real UART/port services."""

import asyncio

from test_native import native_console
from test_python import completed, output
from test_python_hub import BUILD, ready, shell

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Start, Stop, checked, upload
from brickphyrdev.telemetry import snapshot


async def port_matches(client, **fields):
    async with asyncio.timeout(3):
        while True:
            value = await snapshot(client, "port.A")
            if all(value.values.get(key) == expected for key, expected in fields.items()):
                return value
            await asyncio.sleep(0.005)


def test_raw_modes_async_hotplug_and_passive_input():
    source = b"""
from pybricks.iodevices import PUPDevice, LUMPDevice
from pybricks.parameters import Port
from pybricks.tools import run_task
import errno
assert LUMPDevice is PUPDevice
d = PUPDevice(Port.A)
assert d.info() == {'id': 201, 'modes': (
    ('RAW0', 2, 0), ('RAW1', 2, 1), ('RAW2', 2, 2), ('RAW3', 2, 3))}
expected = ((-128, 127), (-123, 32767), (-2147483648, 2147483647), (1.25, -0.5))
for mode, values in enumerate(expected):
    assert d.read(mode) == values
try:
    d.read(4)
except ValueError:
    pass
else:
    assert False
async def read_again():
    pending = d.read(0)
    try:
        d.read(1)
    except OSError as error:
        assert error.args[0] == errno.EBUSY
    else:
        assert False
    assert await pending == expected[0]
run_task(read_again())
print('ready')
input()
try:
    d.read(1)
except OSError as error:
    assert error.args[0] == errno.ENODEV
else:
    assert False
print('ready')
input()
# A replacement device must require a new Python object, even with the same ID.
try:
    d.info()
except OSError as error:
    assert error.args[0] == errno.ENODEV
else:
    assert False
assert PUPDevice(Port.A).read(2) == expected[2]
print('raw passed')
"""
    touch = b"""
from pybricks.iodevices import PUPDevice
from pybricks.parameters import Port
from pybricks.tools import run_task
d = PUPDevice(Port.A)
assert d.info() == {'id': 5}
assert d.read(0) is False
print('ready')
input()
async def pressed():
    assert await d.read(0) is True
run_task(pressed())
print('touch passed')
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await shell(port, captured, b"pup_peer connect")
                # No diagnostic read or mode warm-up before ordinary first use.
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await shell(port, captured, b"pup_peer unplug")
                await port_matches(client, phase=1)  # EMPTY
                await checked(client, Input(data=b"\n"))
                await ready(client)
                await shell(port, captured, b"pup_peer connect")
                await port_matches(client, phase=7)  # READY
                await checked(client, Input(data=b"\n"))
                result = await completed(client)
                text = await output(client)
                assert result.result == result.fault == result.cleanup == 0, text
                assert text.strip() == b"raw passed"
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0
                await shell(port, captured, b"pup_peer touch")
                await port_matches(client, type_id=5, pressed=0)
                await upload(client, port, touch, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await shell(port, captured, b"pup_peer press")
                await port_matches(client, pressed=1)
                await checked(client, Input(data=b"\n"))
                result = await completed(client)
                text = await output(client)
                assert result.result == result.fault == result.cleanup == 0, text
                assert text.strip() == b"touch passed"
            except BaseException as error:
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_writes_formats_and_forced_stop_release():
    source = b"""
from pybricks.iodevices import PUPDevice
from pybricks.parameters import Port
from pybricks.tools import run_task
d = PUPDevice(Port.A)
for mode, values in enumerate(((-100, 110), (-300, 200),
        (-2147483647, 2147483646), (2.5, -1.25))):
    assert d.write(mode, values) is None
    assert d.read(mode) == values
for mode, values in ((0, (128, 0)), (1, (0, -32769)), (2, (0,)), (4, (0, 0))):
    try:
        d.write(mode, values)
    except ValueError:
        pass
    else:
        assert False
async def copied_data():
    values = [-30, 60]
    pending = d.write(0, values)
    values[0] = 90
    assert await pending is None
    assert await d.read(0) == (-30, 60)
run_task(copied_data())
print('ready')
input()
async def blocked():
    pending = d.write(1, (100, 200))
    print('ready')
    await pending
    assert False, 'Peer ignored SELECT; write must not complete'
run_task(blocked())
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await shell(port, captured, b"pup_peer connect")
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await shell(port, captured, b"pup_peer hold")
                await checked(client, Input(data=b"\n"))
                await ready(client)
                await checked(client, Stop())
                result = await completed(client, state=3)
                assert result.fault == result.cleanup == 0
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0
                await shell(port, captured, b"pup_peer resume")
                await upload(client, port, b"""
from pybricks.iodevices import PUPDevice
from pybricks.parameters import Port
d = PUPDevice(Port.A)
d.write(2, (123, -456))
assert d.read(2) == (123, -456)
print('replacement passed')
""", Format.PYTHON)
                await checked(client, Start())
                result = await completed(client)
                text = await output(client)
                assert result.result == result.fault == result.cleanup == 0, text
                assert text.strip() == b"replacement passed"
            except BaseException as error:
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))
