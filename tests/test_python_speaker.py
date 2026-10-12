# SPDX-License-Identifier: Apache-2.0
"""Speaker bindings through protected ARM execution and synthetic DMA."""

import asyncio

import pytest
from test_native import native_console
from test_python import completed
from test_python_hub import BUILD, ready, shell
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


def test_speaker_arguments_notes_and_waits():
    assert (
        execute(b"""
from pybricks.hubs import PrimeHub
from pybricks.tools import StopWatch, wait, run_task, multitask
hub = PrimeHub()
s = hub.speaker
assert s.volume() == 100
s.volume(-1)
assert s.volume() == 0
s.volume(150)
assert s.volume() == 100
s.volume(7.9)
assert s.volume() == 7
assert PrimeHub().speaker is s
for operation, error in (
    (lambda: s.volume(float('nan')), ValueError),
    (lambda: s.beep(float('inf')), OverflowError),
    (lambda: s.beep(duration=float('nan')), ValueError),
    (lambda: s.play_notes([], tempo=0), ValueError),
    (lambda: s.play_notes([], tempo=-1), ValueError),
    (lambda: s.play_notes(None), TypeError),
):
    try:
        operation()
    except error:
        pass
    else:
        assert False
for note in ('', 'C', 'C#', 'C4', 'C4/', 'C4/0', 'C4/00', 'C4/1x',
             'R', 'R0/4', 'R/0', 'Cb4/4', 'Fb4/4', 'E#4/4', 'B#4/4',
             'H4/4', 'A1/4', 'A9/4', 'C4/100', 'C4/4' + chr(0)):
    try:
        s.play_notes([note], tempo=24000)
    except ValueError:
        pass
    else:
        assert False, note
s.beep(duration=0)
s.play_notes([])
s.play_notes(['C4/4', 'D#4/8.', 'Bb3/16_', 'R/4'], tempo=240000)
clock = StopWatch()
s.play_notes(['R/4'], tempo=600)
assert clock.time() >= 100
ticks = 0
async def other():
    global ticks
    for i in range(5):
        ticks += 1
        await wait(1)
async def music():
    await s.play_notes((x for x in ['C4/4', 'Db4/8', 'R/4', 'A4/4_']), tempo=6000)
    await s.beep(500, 10)
async def main():
    await multitask(music(), other())
    assert ticks == 5
    await multitask(s.play_notes(['R/1'], tempo=1), wait(10), race=True)
run_task(main())
print('speaker passed')
""")
        == b"speaker passed\n"
    )


def test_notes_use_shared_tone_service_and_hold_pauses():
    source = b"""
import gc
from pybricks.hubs import PrimeHub
s = PrimeHub().speaker
s.volume(25)
def notes():
    for note in ['A4/4', 'C#4/8.', 'Db4/8.', 'Bb3/4_', 'R/4']:
        yield note
        gc.collect()
        print('ready')
        input()
s.play_notes(notes(), tempo=600)
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
                for frequency, on_ms in ((440, 87), (277, 65), (277, 65), (233, 100), (0, 87)):
                    await ready(client)
                    sound = await snapshot(client, "sound")
                    assert sound.error == 0 and sound.values["cleanup"] == 0
                    assert sound.values["owner"] == 1 and sound.values["phase"] == 0
                    assert sound.values["frequency"] == frequency
                    assert sound.values["duration_ms"] == on_ms
                    assert sound.values["volume"] == 25
                    assert sound.values["completed"] == on_ms * 48
                    await shell(port, captured, b"sound tone 500 10 10")
                    assert (await snapshot(client, "sound")).values == sound.values
                    await checked(client, Input(data=b"\n"))
                assert (await completed(client)).result == 0
                assert (await snapshot(client, "sound")).values["owner"] == 0
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


def test_speaker_replacement_interrupt_and_iterator_failure():
    source = b"""
from pybricks.hubs import PrimeHub
from pybricks.tools import wait, run_task, multitask
s = PrimeHub().speaker
s.volume(10)
async def main():
    old = s.beep(440, 10000)
    s.beep(880, -1)
    old.close()
    def replacing_notes():
        s.beep(880, -1)
        yield 'A4/4'
    await s.play_notes(replacing_notes())
    def failing_replacement():
        s.beep(880, -1)
        raise ValueError('replaced')
        yield 'A4/4'
    try:
        await s.play_notes(failing_replacement())
    except ValueError as error:
        assert str(error) == 'replaced'
    else:
        assert False
    print('ready')
    input()
    await s.beep(0, 0)
    await multitask(s.beep(440, 10000), wait(5), race=True)
run_task(main())
print('ready')
input()
try:
    s.beep(440, 10000)
except KeyboardInterrupt:
    print('ready')
    input()
def broken():
    yield 'A4/64'
    raise ValueError('note source')
async def broken_async():
    try:
        await s.play_notes(broken())
    except ValueError as error:
        assert str(error) == 'note source'
    else:
        assert False
run_task(broken_async())
print('ready')
input()
s.beep(1234, -1)
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
                sound = await snapshot(client, "sound")
                assert sound.values["frequency"] == 880 and sound.values["owner"] == 1
                await checked(client, Input(data=b"\n"))
                await ready(client)
                assert (await snapshot(client, "sound")).values["owner"] == 0
                await checked(client, Input(data=b"\n"))
                async with asyncio.timeout(2):
                    while (await snapshot(client, "sound")).values["owner"] != 1:
                        await asyncio.sleep(0.01)
                await checked(client, Interrupt())
                await ready(client)
                assert (await snapshot(client, "sound")).values["owner"] == 0
                await checked(client, Input(data=b"\n"))
                await ready(client)
                assert (await snapshot(client, "sound")).values["owner"] == 0
                await checked(client, Input(data=b"\n"))
                await ready(client)
                sound = await snapshot(client, "sound")
                assert sound.values["frequency"] == 1234 and sound.values["owner"] == 1
                await shell(port, captured, b"session_close")
                assert (await snapshot(client, "sound")).values["owner"] == 1
            finally:
                await checked(client, Stop())
            sound = await snapshot(client, "sound")
            assert sound.values["owner"] == 0 and sound.values["cleanup"] == 0

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))


@pytest.mark.parametrize("stop_error", [0, 1])
def test_speaker_busy_driver_failure_and_repl_reset(stop_error):
    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (
            port,
            client,
        ):
            try:
                await shell(port, captured, b"sound tone 1000 0 10")
                await checked(client, Repl())
                await until(client, b">>> ")
                await checked(
                    client,
                    Input(
                        data=b"from pybricks.hubs import PrimeHub; s=PrimeHub().speaker; s.beep()\r"
                    ),
                )
                assert b"EBUSY" in await until(client, b">>> ")
                assert (await snapshot(client, "sound")).values["owner"] == 2
                await shell(port, captured, b"sound stop")
                await shell(port, captured, f"sound_error 1 {stop_error}".encode())
                await checked(client, Input(data=b"s.beep()\r"))
                assert b"EIO" in await until(client, b">>> ")
                sound = await snapshot(client, "sound")
                assert sound.error != 0
                assert sound.values["owner"] == stop_error
                await shell(port, captured, b"sound_error 0 0")
                await checked(client, Input(data=b"s.beep(440, -1)\r"))
                assert b"Traceback" not in await until(client, b">>> ")
                assert (await snapshot(client, "sound")).values["owner"] == 1
                await checked(client, Input(data=b"\x04"))
                assert b"MPY: soft reboot" in await until(client, b">>> ")
                assert (await checked(client, Status())).cleanup == 0
                assert (await snapshot(client, "sound")).values["owner"] == 0
            finally:
                await shell(port, captured, b"sound_error 0 0")
                await shell(port, captured, b"sound stop")
                await checked(client, Stop())

    with native_console(BUILD) as device:
        asyncio.run(exercise(device))
