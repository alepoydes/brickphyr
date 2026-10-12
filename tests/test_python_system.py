# SPDX-License-Identifier: Apache-2.0
"""System bindings through ARM execution, synthetic buttons and real lifecycle."""

import asyncio

from test_native import native_console
from test_python import completed, output
from test_python_hub import BUILD, ready, shell
from test_python_io import until
from test_python_modules import execute

from brickphyrdev import console
from brickphyrdev.application import Format, Input, Repl, Start, Stop, checked, upload
from brickphyrdev.telemetry import snapshot


def test_system_information_and_validation():
    assert (
        execute(b"""
import errno
from pybricks.hubs import PrimeHub
from pybricks.parameters import Button
s = PrimeHub().system
assert s.name() == 'Brickphyr'
assert s.info() == {'name': 'Brickphyr', 'host_connected_usb': False,
    'host_connected_ble': False, 'program_id': -1, 'program_start_type': 3}
try:
    s.reset_reason()
except OSError as error:
    assert error.errno == errno.ENOTSUP  # No reset-cause device on this ARM model.
else:
    assert False
for buttons in (None, False, [], Button.CENTER, [Button.LEFT, Button.RIGHT]):
    s.set_stop_button(buttons)
for buttons, error in ((Button.A, ValueError), (True, TypeError), (['LEFT'], TypeError)):
    try:
        s.set_stop_button(buttons)
    except error:
        pass
    else:
        assert False
print('system passed')
""")
        == b"system passed\n"
    )


def test_stop_combination_catch_disable_and_default():
    source = b"""
import gc
from pybricks.hubs import PrimeHub
from pybricks.parameters import Button, Color
h = PrimeHub()
h.light.on(Color.RED)
h.speaker.beep(440, -1)
h.system.set_stop_button((Button.LEFT, Button.RIGHT))
def broken():
    yield Button.CENTER
    raise RuntimeError('button source')
try:
    h.system.set_stop_button(broken())
except RuntimeError as e:
    assert str(e) == 'button source'
else:
    assert False
# A partial combination must not stop, and a rejected replacement preserves it.
print('ready')
input()
try:
    print('ready')
    input()
except SystemExit as e:
    saved = e
else:
    assert False
gc.collect()
assert isinstance(saved, SystemExit)
h.system.set_stop_button(None)
print('ready')
input()
h.system.set_stop_button(Button.CENTER)
print('ready')
while True: pass
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (
            port,
            client,
        ):
            try:
                await shell(port, captured, b"button_input 0")
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await shell(port, captured, b"button_input 2")
                await checked(client, Input(data=b"\n"))
                await ready(client)
                await shell(port, captured, b"button_input 6")
                await ready(client)
                await shell(port, captured, b"button_input 0")
                await shell(port, captured, b"button_input 1")
                await checked(client, Input(data=b"\n"))
                await ready(client)
                # Setting CENTER while it is down requires a fresh press.
                await shell(port, captured, b"button_input 0")
                await shell(port, captured, b"button_input 1")
                result = await completed(client)
                assert result.result == result.cleanup == 0
                assert b"Traceback" not in await output(client)
                assert (await snapshot(client, "sound")).values["owner"] == 0
                # A fresh VM restores CENTER even after disabling it previously.
                await shell(port, captured, b"button_input 0")
                await upload(client, port, b"print('ready')\nwhile True: pass\n", Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                await shell(port, captured, b"button_input 1")
                assert (await completed(client)).result == 0
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_repl_launch_identity():
    async def exercise(device):
        async with console.connection(lambda _: None, port=device, management=True) as (_, client):
            try:
                await checked(client, Repl())
                await until(client, b">>> ")
                await checked(
                    client,
                    Input(
                        data=b"from pybricks.hubs import PrimeHub; print(PrimeHub().system.info()['program_id'])\r"
                    ),
                )
                assert b"\r\n128\r\n" in await until(client, b">>> ")
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_shutdown_requests_supervised_cleanup():
    async def exercise(device):
        async with console.connection(lambda _: None, port=device, management=True) as (
            port,
            client,
        ):
            await upload(
                client,
                port,
                b"""
from pybricks.hubs import PrimeHub
h = PrimeHub()
h.speaker.beep(440, -1)
h.display.on()
h.system.shutdown()
print('must not return')
""",
                Format.PYTHON,
            )
            await checked(client, Start())
            state = await completed(client, state=3)
            assert state.cleanup == 0
            assert b"must not return" not in await output(client)
            assert (await snapshot(client, "power")).values["shutdown"] == 1
            assert (await snapshot(client, "sound")).values["owner"] == 0
            assert (await snapshot(client, "matrix")).values["owner"] == 0

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))
