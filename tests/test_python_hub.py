# SPDX-License-Identifier: Apache-2.0
"""Pybricks hub bindings through ARM user mode, real services and synthetic I/O."""

import asyncio
import re
from pathlib import Path

import pytest
from test_native import native_console
from test_python import completed, output
from test_python_io import until
from test_python_modules import execute

from brickphyrdev import console
from brickphyrdev.application import (
    Format,
    Input,
    Interrupt,
    Repl,
    Start,
    Status,
    Stop,
    checked,
    upload,
)
from brickphyrdev.telemetry import snapshot

BUILD = Path(__file__).resolve().parents[1] / "build/python-smp-tests"


async def shell(port, captured, command):
    captured.clear()
    await port.write(command + b"\r")
    async with asyncio.timeout(3):
        while not captured.endswith(b"brickphyr> "):
            await asyncio.sleep(0.01)
    return bytes(captured)


async def frame(port, captured):
    reply = await shell(port, captured, b"led_frame")
    match = re.search(rb"SPI: ([0-9a-fA-F]{194})", reply)
    assert match, reply
    data = bytes.fromhex(match[1].decode())
    # TLC5955's observed wire words, not a second implementation of rendering.
    return [int.from_bytes(data[i : i + 2], "big") for i in range(1, 97, 2)]


async def ready(client):
    text = bytearray()
    async with asyncio.timeout(3):
        while not text.endswith(b"ready\r\n"):
            text.extend(await output(client))
            assert (await checked(client, Status())).state == 1, bytes(text)
            await asyncio.sleep(0.01)
    assert text.strip() == b"ready", bytes(text)


def test_hub_identity_buttons_and_battery():
    assert (
        execute(b"""
from pybricks.hubs import PrimeHub, InventorHub, ThisHub
from pybricks.parameters import Button
from pybricks.tools import wait
assert PrimeHub is InventorHub is ThisHub
hub = PrimeHub()
assert hub.buttons.pressed() == {Button.CENTER, Button.RIGHT}
try:
    hub.buttons = None
    assert False
except AttributeError:
    pass
assert hub.battery.type() == 'Li-ion'
wait(1050)
assert 7000 < hub.battery.voltage() < 9000
assert hub.battery.current() == 1825
assert 24000 <= hub.battery.temperature() <= 26000
print('hub observations passed')
""")
        == b"hub observations passed\n"
    )


def test_display_rendering_and_validation():
    # Reference font A from Pybricks' documented 5x5 font, observed as brightness.
    letter = bytes(
        (
            0,
            100,
            100,
            0,
            0,
            100,
            0,
            0,
            100,
            0,
            100,
            100,
            100,
            100,
            0,
            100,
            0,
            0,
            100,
            0,
            100,
            0,
            0,
            100,
            0,
        )
    )
    cases = [
        ("hub.display.char('A')", letter),
        ("hub.display.on(50)", bytes([50] * 25)),
        ("hub.display.off(); hub.display.pixel(0, 0, 100)", bytes([100] + [0] * 24)),
        (
            "hub.display.off(); hub.display.orientation(Side.LEFT); hub.display.pixel(0, 0)",
            bytes([0] * 20 + [100] + [0] * 4),
        ),
        (
            "hub.display.off(); hub.display.orientation(Side.RIGHT); hub.display.pixel(0, 0)",
            bytes([0] * 4 + [100] + [0] * 20),
        ),
        (
            "hub.display.off(); hub.display.orientation(Side.BOTTOM); hub.display.pixel(0, 0)",
            bytes([0] * 24 + [100]),
        ),
        ("other = PrimeHub(); hub.display.off(); hub.display.pixel(0, 0)", bytes([100] + [0] * 24)),
        (
            "other.display.orientation(Side.RIGHT); hub.display.off(); hub.display.pixel(0, 0)",
            bytes([0] * 4 + [100] + [0] * 20),
        ),
        ("hub.display.orientation(Side.TOP); hub.display.icon([[150]*5]*5)", bytes([100] * 25)),
        ("hub.display.icon(Matrix([[25.9]*5]*5))", bytes([25] * 25)),
        ("hub.display.pixel(-1, 0); hub.display.pixel(5, 5)", bytes([25] * 25)),
    ]
    source = """
from pybricks.hubs import PrimeHub
from pybricks.parameters import Side
from pybricks.tools import Matrix
hub = PrimeHub()
"""
    for command, _ in cases:
        source += command + "\nprint('ready')\ninput()\n"
    source += """
for operation in (
    lambda: hub.display.icon([[0]*5]*4),
    lambda: hub.display.icon([[0]*5]*4 + [[0]*4]),
    lambda: hub.display.icon(Matrix([[0]*5]*4)),
    lambda: hub.display.icon(Matrix([[float('nan')]*5]*5)),
    lambda: hub.display.on(float('nan')),
    lambda: hub.display.char('ab'),
    lambda: hub.display.char('é'),
):
    try:
        operation()
        assert False
    except ValueError:
        pass
print('ready')
input()
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (
            port,
            client,
        ):
            try:
                await upload(client, port, source.encode(), Format.PYTHON)
                await checked(client, Start())
                for _, pixels in cases + [("invalid input leaves frame unchanged", cases[-1][1])]:
                    await ready(client)
                    display = await snapshot(client, "matrix")
                    assert display.error == 0 and display.values["owner"] == 1
                    assert display.values["applied"] == pixels
                    await checked(client, Input(data=b"\n"))
                assert (await completed(client)).result == 0
                display = await snapshot(client, "matrix")
                assert display.values["owner"] == 0 and display.values["applied"] == bytes(25)
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


@pytest.mark.parametrize("ending, animated", [("return", False), ("exception", True), ("stop", True)])
def test_lighting_and_display_cleanup(ending, animated):
    source = b"""
from pybricks.hubs import PrimeHub
from pybricks.parameters import Color
hub = PrimeHub()
hub.light.on(Color.RED)
hub.bluetooth_light.on(Color.BLUE)
hub.display.on()
print('ready')
input()
"""
    if animated:
        source = source.replace(b"hub.light.on(Color.RED)", b"hub.light.animate([Color.RED], 10)")
        source = source.replace(
            b"hub.bluetooth_light.on(Color.BLUE)", b"hub.bluetooth_light.blink(Color.BLUE, [10])"
        )
        source = source.replace(b"hub.display.on()", b"hub.display.animate([[[100]*5]*5]*2, 10)")
    if ending == "exception":
        source += b"raise ValueError('device cleanup')\n"
    elif ending == "stop":
        source += b"while True: pass\n"

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (
            port,
            client,
        ):
            try:
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                wire = await frame(port, captured)
                assert wire[5] > 60000 and wire[4] == wire[3] == 0
                assert wire[8] == wire[5]  # Both central LEDs share one color.
                assert wire[18] > 15000 and wire[19] == wire[20] == 0
                assert wire[38] == 65535 and wire[14] == 65535
                await checked(client, Input(data=b"\n"))
                if ending == "stop":
                    await checked(client, Stop())
                    status = await completed(client, 3)
                else:
                    status = await completed(client)
                    assert status.result == (-8 if ending == "exception" else 0)
                assert status.cleanup == 0
                wire = await frame(port, captured)
                assert wire[5] == 0 and wire[3] > 0  # System cyan restored.
                assert wire[18] == wire[19] == wire[20] == wire[38] == 0
                assert (await snapshot(client, "matrix")).values["owner"] == 0
                await upload(
                    client,
                    port,
                    b"""
from pybricks.hubs import PrimeHub
hub = PrimeHub()
hub.light.off()
hub.bluetooth_light.off()
hub.display.number(-42)
""",
                    Format.PYTHON,
                )
                await checked(client, Start())
                assert (await completed(client)).result == 0
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_light_animations_validate_copy_and_run_without_python_yields():
    source = b"""
import gc
from pybricks.hubs import PrimeHub
from pybricks.parameters import Color
hub = PrimeHub()
colors = [Color.RED]
hub.light.animate(colors=colors, interval=10000)
colors[0] = Color.GREEN
durations = [10000]
hub.bluetooth_light.blink(color=Color.BLUE, durations=durations)
durations[0] = 0
del colors, durations
gc.collect()
for operation, error in (
    (lambda: hub.light.animate([], 10), ValueError),
    (lambda: hub.light.animate([Color.BLUE], 0), ValueError),
    (lambda: hub.light.animate([Color.BLUE], -1), ValueError),
    (lambda: hub.light.animate([Color.BLUE], float('nan')), ValueError),
    (lambda: hub.light.animate([Color.BLUE], float('inf')), OverflowError),
    (lambda: hub.light.animate([Color.BLUE, 7], 10), TypeError),
    (lambda: hub.light.blink(Color.BLUE, [10, 0]), ValueError),
    (lambda: hub.light.blink(Color.BLUE, [10, -1]), ValueError),
    (lambda: hub.light.blink(Color.BLUE, []), ValueError),
    (lambda: hub.light.animate([Color.BLUE] * 257, 10), ValueError),
):
    try:
        operation()
    except error:
        pass
    else:
        assert False
gc.collect()
print('ready')
input()
hub.light.animate([Color.RED, Color.BLUE], 60)
hub.bluetooth_light.blink(Color.GREEN, [100, 100, 100])
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
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                wire = await frame(port, captured)
                assert wire[5] > 60000 and wire[4] == wire[3] == 0
                assert wire[18] > 15000 and wire[19] == wire[20] == 0
                await checked(client, Input(data=b"\n"))
                await ready(client)
                colors, blinking = set(), set()
                async with asyncio.timeout(3):
                    while len(colors) != 2 or len(blinking) != 2:
                        wire = await frame(port, captured)
                        assert wire[4] == wire[18] == wire[20] == 0
                        colors.add((bool(wire[5]), bool(wire[3])))
                        blinking.add(bool(wire[19]))
                        await asyncio.sleep(0.017)
                assert colors == {(True, False), (False, True)}
                assert blinking == {False, True}
                assert (await checked(client, Status())).state == 1
            finally:
                await checked(client, Stop())
            wire = await frame(port, captured)
            assert wire[5] == wire[18] == wire[19] == wire[20] == 0
            assert wire[3] > 0

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_display_owner_and_io_error():
    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (
            port,
            client,
        ):
            try:
                await shell(port, captured, b"matrix fill 50")
                await upload(
                    client,
                    port,
                    b"""
import errno
from pybricks.hubs import PrimeHub
from pybricks.parameters import Color
hub = PrimeHub()
try:
    hub.display.on()
    assert False
except OSError as error:
    assert error.errno == errno.EBUSY
print('ready')
input()
for operation in (lambda: hub.display.on(), lambda: hub.light.on(Color.RED)):
    try:
        operation()
        assert False
    except OSError as error:
        assert error.errno == errno.EIO
print('ready')
input()
hub.display.on()
hub.light.on(Color.GREEN)
""",
                    Format.PYTHON,
                )
                await checked(client, Start())
                await ready(client)
                display = await snapshot(client, "matrix")
                assert display.values["owner"] == 2
                assert display.values["applied"] == bytes([50] * 25)
                await shell(port, captured, b"matrix clear")
                await shell(port, captured, b"led_error 1")
                await checked(client, Input(data=b"\n"))
                await ready(client)
                await shell(port, captured, b"led_error 0")
                await checked(client, Input(data=b"\n"))
                assert (await completed(client)).result == 0
            finally:
                await shell(port, captured, b"led_error 0")
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_repl_reset_releases_hub_devices():
    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (
            port,
            client,
        ):
            try:
                await checked(client, Repl())
                await until(client, b">>> ")
                for command in (
                    b"from pybricks.hubs import PrimeHub; from pybricks.parameters import Color, Side",
                    b"hub = PrimeHub(); hub.light.animate([Color.RED], 10); "
                    b"hub.bluetooth_light.blink(Color.BLUE, [10])",
                    b"hub.display.orientation(Side.RIGHT); "
                    b"hub.display.animate([[[100,0,0,0,0]]+[[0]*5]*4]*2, 10)",
                ):
                    await checked(client, Input(data=command + b"\r"))
                    assert b"Traceback" not in await until(client, b">>> ")
                assert (await snapshot(client, "matrix")).values["applied"][4] == 100
                assert (await frame(port, captured))[5] > 60000
                await checked(client, Input(data=b"\x04"))
                assert b"MPY: soft reboot" in await until(client, b">>> ")
                assert (await checked(client, Status())).cleanup == 0
                assert (await snapshot(client, "matrix")).values["owner"] == 0
                wire = await frame(port, captured)
                assert wire[5] == wire[18] == wire[19] == wire[20] == wire[33] == 0
                await checked(
                    client,
                    Input(
                        data=b"from pybricks.hubs import PrimeHub; PrimeHub().display.pixel(0, 0)\r"
                    ),
                )
                assert b"Traceback" not in await until(client, b">>> ")
                assert (await snapshot(client, "matrix")).values["applied"] == bytes(
                    [100] + [0] * 24
                )
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_matrix_animation_copy_orientation_and_busy_python():
    source = b"""
import gc
from pybricks.hubs import PrimeHub
from pybricks.parameters import Side
hub = PrimeHub()
icon = [[100,0,0,0,0]] + [[0]*5]*4
hub.display.animate([icon, icon], 50)
icon[0][0] = 0
del icon
gc.collect()
for operation, error in (
    (lambda: hub.display.animate([], 10), ValueError),
    (lambda: hub.display.animate([[[0]*5]*5], 10), ValueError),
    (lambda: hub.display.animate([[[0]*5]*5]*256, 10), ValueError),
    (lambda: hub.display.animate([[[0]*5]*5]*2, 0), ValueError),
    (lambda: hub.display.animate([[[0]*5]*5]*2, float('inf')), OverflowError),
    (lambda: hub.display.animate([[[0]*5]*5, [[0]*4]*5], 10), ValueError),
    (lambda: hub.display.text('x' + chr(0)), ValueError),
    (lambda: hub.display.text('x', on=-1), ValueError),
    (lambda: hub.display.text('x', off=float('nan')), ValueError),
):
    try:
        operation()
    except error:
        pass
    else:
        assert False
print('ready')
input()
hub.display.orientation(Side.RIGHT)
print('ready')
input()
hub.display.pixel(2, 2, 50)
print('ready')
input()
hub.display.animate([[[25]*5]*5, [[75]*5]*5], 50)
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
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                assert (await snapshot(client, "matrix")).values["applied"] == bytes(
                    [100] + [0] * 24
                )
                await checked(client, Input(data=b"\n"))
                await ready(client)
                async with asyncio.timeout(1):
                    while (await snapshot(client, "matrix")).values["applied"][4] != 100:
                        await asyncio.sleep(0.01)
                await checked(client, Input(data=b"\n"))
                await ready(client)
                assert (await snapshot(client, "matrix")).values["applied"] == bytes(
                    [0] * 12 + [50] + [0] * 12
                )
                await checked(client, Input(data=b"\n"))
                await ready(client)
                seen = set()
                async with asyncio.timeout(2):
                    while len(seen) < 2:
                        seen.add((await snapshot(client, "matrix")).values["applied"])
                        await asyncio.sleep(0.017)
                assert seen == {bytes([25] * 25), bytes([75] * 25)}
            finally:
                await checked(client, Stop())
            assert (await snapshot(client, "matrix")).values["owner"] == 0

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_display_text_wait_and_cancellation():
    assert (
        execute(b"""
import gc
from pybricks.hubs import PrimeHub
from pybricks.tools import wait, run_task, multitask, StopWatch
hub = PrimeHub()
clock = StopWatch()
hub.display.text('AB', on=15, off=5)
assert clock.time() >= 40
hub.display.text('', on=0, off=0)
hub.display.text('AB', on=0, off=0)
ticks = 0
async def other():
    global ticks
    for i in range(8):
        gc.collect()
        ticks += 1
        await wait(5)
async def show():
    await hub.display.text(''.join(['A', 'B']), on=25, off=5)
async def main():
    await multitask(show(), other())
    assert ticks == 8
    await hub.display.text('', on=0, off=0)
    # A newer operation exhausts the old awaitable, whose close must not
    # cancel the replacement. Explicit close of the new operation is idempotent.
    old = hub.display.text('A', on=1000)
    next(old)
    await wait(5)
    new = hub.display.text('B', on=1000)
    old.close()
    next(new)
    await wait(5)
    new.close()
    new.close()
    # Complete-multitask cancellation also blanks its unfinished display.
    await multitask(hub.display.text('C', on=1000), wait(10), race=True)
run_task(main())
print('text passed')
""")
        == b"text passed\n"
    )


def test_display_text_frames_and_static_replacement():
    source = b"""
from pybricks.hubs import PrimeHub
from pybricks.tools import wait, run_task, multitask
hub = PrimeHub()
async def replacement():
    await wait(40)
    print('ready')
    input()
    hub.display.pixel(4, 4, 50)
async def main():
    await multitask(hub.display.text('A', on=10000), replacement())
run_task(main())
print('ready')
input()
async def cancelled():
    await multitask(hub.display.text('B', on=10000), wait(40), race=True)
run_task(cancelled())
print('ready')
input()
try:
    hub.display.text('A', on=10000)
except KeyboardInterrupt:
    print('ready')
    input()
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (
            port,
            client,
        ):
            try:
                await upload(client, port, source, Format.PYTHON)
                await checked(client, Start())
                await ready(client)
                shown = (await snapshot(client, "matrix")).values["applied"]
                assert shown[1:3] == bytes([100, 100]) and shown[24] == 0
                await checked(client, Input(data=b"\n"))
                await ready(client)
                assert (await snapshot(client, "matrix")).values["applied"] == shown[:24] + bytes(
                    [50]
                )
                await checked(client, Input(data=b"\n"))
                await ready(client)
                assert (await snapshot(client, "matrix")).values["applied"] == bytes(25)
                await checked(client, Input(data=b"\n"))
                async with asyncio.timeout(1):
                    while (await snapshot(client, "matrix")).values["applied"] == bytes(25):
                        await asyncio.sleep(0.01)
                await checked(client, Interrupt())
                await ready(client)
                assert (await snapshot(client, "matrix")).values["applied"] == bytes(25)
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))
