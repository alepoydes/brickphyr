# SPDX-License-Identifier: Apache-2.0
"""Upstream hub menu through protected Python, shared matrix and button events."""

import asyncio
import errno

from test_native import native_console
from test_python import completed, output
from test_python_hub import BUILD, ready, shell

from brickphyrdev import console
from brickphyrdev.application import Format, Interrupt, Start, Status, Stop, checked, upload
from brickphyrdev.telemetry import snapshot

# Reference 5x5 A; only used to synchronize the menu after initial validation.
LETTER_A = bytes(
    100 if c == "#" else 0
    for row in (" ##  ", "#  # ", "#### ", "#  # ", "#  # ")
    for c in row
)


async def displayed(client, *, expected=None, different=None):
    async with asyncio.timeout(3):
        while True:
            value = await snapshot(client, "matrix")
            assert value.error in (0, -errno.ENODATA), value
            pixels = value.values["applied"]
            if (
                value.error == 0 and value.values["owner"] == 1
                and not value.values["pending"] and any(pixels)
            ):
                if (
                    (expected is None or pixels == expected)
                    and (different is None or pixels != different)
                ):
                    return pixels
            assert (await checked(client, Status())).state == 1
            await asyncio.sleep(0.001)


async def buttons(port, captured, mask):
    await shell(port, captured, f"button_input {mask}".encode())
    # Button API exposes levels, not queued edges. Hold each level for two
    # physical 10 ms scans so a release/press cannot collapse into one sample.
    await asyncio.sleep(0.02)


def test_menu_navigation_release_and_stop_restoration():
    source = b"""
from pybricks.tools import hub_menu
# No hub construction is required; retain and return the original selected object.
number = 42.0
assert hub_menu('A', number, 'Z') is number
print('ready')
try:
    input()
except SystemExit:
    print('center restored')
else:
    assert False
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await buttons(port, captured, 2)
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                first = await displayed(client, expected=LETTER_A)
                # A held entry button is ignored; a fresh LEFT wraps to the last entry.
                await buttons(port, captured, 0)
                await buttons(port, captured, 2)
                last = await displayed(client, different=first)
                await buttons(port, captured, 0)
                await buttons(port, captured, 4)
                assert await displayed(client, different=last) == first
                await buttons(port, captured, 0)
                await buttons(port, captured, 4)
                selected = await displayed(client, different=first)
                assert selected != last
                await buttons(port, captured, 0)
                await buttons(port, captured, 1)
                assert (await checked(client, Status())).state == 1
                assert await output(client) == b"", "Menu returned before center release"
                await buttons(port, captured, 0)
                await ready(client)
                await buttons(port, captured, 1)
                result = await completed(client)
                assert result.result == result.fault == result.cleanup == 0
                assert (await output(client)).strip() == b"center restored"
                assert (await snapshot(client, "matrix")).values["owner"] == 0
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_menu_errors_and_interrupt_restore_custom_stop():
    source = b"""
from pybricks.hubs import PrimeHub
from pybricks.parameters import Button
from pybricks.tools import hub_menu, run_task
h = PrimeHub()
h.system.set_stop_button((Button.LEFT, Button.RIGHT))
for args, error in ((('A',), TypeError), (('A', 'long'), ValueError)):
    try:
        hub_menu(*args)
    except error:
        pass
    else:
        assert False
async def invalid_context():
    try:
        hub_menu('X', 'Y')
    except RuntimeError:
        pass
    else:
        assert False
run_task(invalid_context())
try:
    hub_menu('A', 'B')
except KeyboardInterrupt:
    print('ready')
else:
    assert False
try:
    input()
except SystemExit:
    print('combination restored')
else:
    assert False
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await buttons(port, captured, 0)
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await displayed(client, expected=LETTER_A)
                await checked(client, Interrupt())
                await ready(client)
                await buttons(port, captured, 6)
                result = await completed(client)
                assert result.result == result.fault == result.cleanup == 0
                assert (await output(client)).strip() == b"combination restored"
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))
