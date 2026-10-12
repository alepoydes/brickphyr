# SPDX-License-Identifier: Apache-2.0
"""Custom UART through protected ARM Python and the shared interrupt driver."""

import asyncio
import errno
import re

from smpclient import success
from test_native import native_console
from test_python import completed, output
from test_python_hub import BUILD, ready, shell
from test_python_io import until

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Start, Status, Stop, checked, upload
from brickphyrdev.telemetry import snapshot


def test_uart_power_consent_continuity_and_failed_open():
    source = b"""
from pybricks.iodevices import UARTDevice
from pybricks.parameters import Port
import errno
try:
    UARTDevice(Port.A, power_pin=1)
except OSError as e:
    assert e.args[0] == errno.ENOTSUP
else:
    assert False, 'power was enabled despite refusal'
print('ready'); input()
d = UARTDevice(Port.A, power_pin=1)
print('ready'); input()
d.set_baudrate(9600)
d.clear()
d.write(b'powered')
assert d.read_all() == b'powered'
print('ready'); input()
"""
    failed_open = b"""
from pybricks.iodevices import UARTDevice
from pybricks.parameters import Port
import errno
try:
    UARTDevice(Port.A, power_pin=2)
except OSError as e:
    assert e.args[0] == errno.EIO
else:
    assert False, 'injected supply failure was ignored'
print('ready'); input()
# The failed constructor released its lease while Python stayed alive.
# Prior acknowledgement survives application exit in RAM, without another prompt.
d = UARTDevice(Port.A, power_pin=2)
d.write(b'recovered')
assert d.read_all() == b'recovered'
print('ready'); input()
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            async def power():
                text = await shell(port, captured, b"pup_peer power")
                match = re.search(rb"supply=(\d+) changes=(\d+)", text)
                assert match, text
                return tuple(map(int, match.groups()))

            try:
                await shell(port, captured, b"pup_peer serial")
                initial = await power()
                assert initial[0] == 0
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await until(client, b"Press N to cancel.\r\n")
                assert await power() == initial
                await checked(client, Input(data=b"N"))
                assert b"Cancelled. Power not enabled." in await until(client, b"ready\r\n")
                assert await power() == initial
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0

                await checked(client, Input(data=b"\n"))
                await until(client, b"Press N to cancel.\r\n")
                await checked(client, Input(data=b"Y"))
                assert b"allowed in RAM" in await until(client, b"ready\r\n")
                powered = await power()
                assert powered[0] == 1 and powered[1] == initial[1] + 1, powered
                assert (await snapshot(client, "port.A")).values["supply"] == 1

                await checked(client, Input(data=b"\n"))
                await ready(client)
                assert await power() == powered, 'baud/clear pulsed device power'
                await checked(client, Input(data=b"\n"))
                result = await completed(client)
                assert result.result == result.fault == result.cleanup == 0, result
                assert (await power())[0] == 0

                await shell(port, captured, b"pup_peer serial_power_error")
                await upload(client, port, failed_open, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                assert (await power())[0] == 0
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0
                await checked(client, Input(data=b"\n"))
                await ready(client)
                powered = await power()
                assert powered[0] == 2, powered
                # Revoking future permission does not silently stop a live device.
                await shell(port, captured, b"config set uart/power_allowed 0")
                assert await power() == powered
                await checked(client, Stop())
                result = await completed(client, state=3)
                assert result.cleanup == 0, result
                assert (await power())[0] == 0
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0
            except BaseException as error:
                error.add_note(f"Python output: {await output(client)!r}")
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_serial_duplex_errors_and_application_release():
    source = b"""
from pybricks.iodevices import UARTDevice
from pybricks.parameters import Port
from pybricks.tools import run_task, multitask
import errno, gc

def error(call, number):
    try:
        call()
    except OSError as e:
        assert e.args[0] == number, e
    else:
        assert False, 'expected error'

d = UARTDevice(Port.A, timeout=300)
assert d.waiting() == 0
# A second Python object cannot steal the live lease.
error(lambda: UARTDevice(Port.A), errno.EBUSY)
assert d.write(bytes(range(96))) is None
assert d.read(96) == bytes(range(96))
assert d.read(0) == b''
d.set_baudrate(9600)
d.write('hello')
assert d.waiting() == 5
assert d.read_all() == b'hello'
assert d.read_all() == b''
error(lambda: d.read(), errno.ETIMEDOUT)

async def duplex():
    data = bytearray(range(96))
    tx = d.write(data)
    data[0] = 255
    del data
    gc.collect()
    async def receive():
        assert await d.read(96) == bytes(range(96))
    await multitask(tx, receive())
    old_read = d.read(10)
    d.clear()
    assert await old_read is None
    await d.write(b'aaabaZ')
    pattern = bytes(bytearray(b'aaba'))
    waiting = d.wait_until(pattern)
    del pattern
    gc.collect()
    assert await waiting is None
    assert await d.read() == b'Z'
run_task(duplex())
# A stalled consumer must see overflow, not silently lose bytes or switch to LUMP.
error(lambda: d.write(bytes(544)), errno.ENOBUFS)
error(d.waiting, errno.ENOBUFS)
d.clear()
d.write(b'recovered')
assert d.read_all() == b'recovered'
print('serial passed')
"""
    blocked = b"""
from pybricks.iodevices import UARTDevice
from pybricks.parameters import Port
d = UARTDevice(Port.A)
print('ready')
d.read()
"""
    replacement = b"""
from pybricks.iodevices import PUPDevice
from pybricks.parameters import Port
d = PUPDevice(Port.A)
assert d.info()['id'] == 201
assert d.read(0) == (-128, 127)
print('LEGO restored')
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await shell(port, captured, b"pup_peer serial")
                # Ordinary first use configures the port; no warm-up or getter.
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                result = await completed(client)
                text = await output(client)
                assert result.result == result.fault == result.cleanup == 0, text
                assert text.strip() == b"serial passed"
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0
                await upload(client, port, blocked, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                # A one-shot coast failure must survive successful secondary
                # cleanup. Status reaps again through the supervisor, allowing
                # a successful retry without erasing the original failure.
                await shell(port, captured, b"pup_peer serial_close_error")
                assert not success(await client.request(Stop()))
                failed = await checked(client, Status())
                assert failed.state == 3 and failed.cleanup == -errno.EIO, failed
                assert (await snapshot(client, "sensor.A")).values["owner"] == 0
                await checked(client, Stop())
                assert (await checked(client, Status())).cleanup == failed.cleanup
                await shell(port, captured, b"pup_peer generic")
                await upload(client, port, replacement, Format.PYTHON)
                await checked(client, Start())
                result = await completed(client)
                text = await output(client)
                assert result.result == result.fault == result.cleanup == 0, text
                assert text.strip() == b"LEGO restored"
            except BaseException as error:
                error.add_note(f"Console: {bytes(captured)!r}")
                raise

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))
