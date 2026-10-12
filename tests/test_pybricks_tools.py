# SPDX-License-Identifier: Apache-2.0
"""Reference Pybricks tools through the protected Python application engine."""

import asyncio
from pathlib import Path

import pytest
from test_native import native_console
from test_python import completed, output
from test_python_modules import execute

from brickphyrdev.application import (
    Format,
    Input,
    Interrupt,
    Start,
    Status,
    Stop,
    checked,
    connection,
    upload,
)

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT.parent / "pybricks-micropython/tests/virtualhub"


@pytest.mark.parametrize(
    "name",
    [
        "color/basic.py",
        "color/invalid_float.py",
        "multitasking/basic.py",
        "multitasking/cleanup_error.py",
        "tools/cobs.py",
        "tools/matrix_print.py",
    ],
)
def test_reference_program(name, python_console):
    path = UPSTREAM / name
    assert execute(path.read_bytes(), port=python_console) == path.with_suffix(".py.exp").read_bytes()


def test_parameters_geometry_and_gc(python_console):
    assert (
        execute(b"""
import gc
from pybricks.parameters import Axis, Button, Color, Direction, Icon, Port, Side, Stop
from pybricks.tools import Matrix, vector, cross
from pybricks.geometry import Matrix as OldMatrix
assert OldMatrix is Matrix
assert str(Port.F) == 'Port.F' and Port.A != Port.B
assert str(Direction.CLOCKWISE) == 'Direction.CLOCKWISE'
assert str(Stop.HOLD) == 'Stop.HOLD' and str(Side.TOP) == 'Side.TOP'
assert Button.CENTER == Button.CENTER and Button.CENTER != Button.BLUETOOTH
assert tuple(Color(-30, 150, -120)) == (330, 100, -100)
Color.CUSTOM = Color(123, 45, 67)
gc.collect()
assert tuple(Color.CUSTOM) == (123, 45, 67)
assert {Color(0): 42}[Color.RED] == 42
assert Icon.HEART.shape == (5, 5)
assert Matrix([[7]]) == 7
a = Matrix([[1, 2, 3], [4, 5, 6]])
assert a.shape == (2, 3) and a.T.shape == (3, 2)
assert tuple(a * vector(1, 0, -1)) == (-2, -2)
assert tuple(cross(Axis.X, Axis.Y)) == (0, 0, 1)
assert abs(vector(3, 4, 0)) == 5
assert (a * a.T)[0, 1] == 32
assert (2 * a + a)[1, 2] == 18
assert (a / 2)[0, 2] == 1.5
gc.collect()
assert tuple(-Axis.Z) == (0, 0, -1)
assert tuple(a.T.T) == (1, 2, 3, 4, 5, 6)
for rows in ([], [[]], [[1, 2], [3]]):
    try:
        Matrix(rows)
        assert False
    except ValueError:
        pass
try:
    a * Axis.X.T
    assert False
except ValueError:
    pass
print('geometry passed')
""", port=python_console)
        == b"geometry passed\n"
    )


def test_clocks_and_parallel_waits(python_console):
    # The reference virtualhub wait case assumes an exact, synthetic clock.
    # Here production time may advance during scheduling, so check deadlines
    # and pause/reset semantics without equating elapsed time to requested time.
    assert (
        execute(b"""
import gc
from pybricks.tools import StopWatch, wait, multitask, run_task
clock = StopWatch()
assert clock.running()
wait(12)
assert clock.time() >= 12
clock.pause()
paused = clock.time()
wait(5)
assert clock.time() == paused and not clock.running()
clock.reset()
assert clock.time() == 0 and not clock.running()
clock.resume()
wait(-1)
assert clock.running()
async def one(i):
    before = clock.time()
    await wait(5 + i)
    assert clock.time() - before >= 5 + i
    gc.collect()
    await wait(0)
    return i
async def main():
    assert run_task()
    for _ in range(2):
        assert await multitask(*(one(i) for i in range(9))) == tuple(range(9))
    await wait(-1)
assert not run_task()
run_task(main())
assert not run_task()
print('clocks passed')
""", port=python_console)
        == b"clocks passed\n"
    )


@pytest.mark.parametrize("mode", ["blocking", "async", "iterator"])
def test_wait_interruption_and_input(mode):
    wait = {
        "blocking": b"wait(60000)",
        "async": b"run_task(waiting())",
        "iterator": b"run_task(Waiting())",
    }[mode]
    source = (
        b"""
from pybricks.tools import wait, run_task, read_input_byte
class Waiting:
    def __iter__(self):
        return self
    def __next__(self):
        return None
    def close(self):
        print('closed')
        raise ValueError('cleanup')
async def waiting():
    await wait(60000)
try:
    print('waiting')
    """
        + wait
        + b"""
except KeyboardInterrupt:
    assert not run_task()
    print('interrupted')
while read_input_byte() != 65:
    wait(1)
while read_input_byte(chr=True) != 'B':
    wait(1)
print('input passed')
"""
    )

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                await upload(client, transport, source, Format.PYTHON)
                await checked(client, Start())
                async with asyncio.timeout(3):
                    while not (await checked(client, Status())).queued:
                        await asyncio.sleep(0.01)
                assert (await checked(client, Status())).state == 1
                await checked(client, Interrupt())
                await checked(client, Input(data=b"AB"))
                status = await completed(client)
                text = await output(client)
                assert status.result == 0, (status, text)
                closed = b"closed\r\n" if mode == "iterator" else b""
                assert text == b"waiting\r\n" + closed + b"interrupted\r\ninput passed\r\n"
            finally:
                await checked(client, Stop())

    with native_console(ROOT / "build/python-smp-tests") as port:
        asyncio.run(exercise(port))


def test_forced_stop_resets_library_state():
    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                await upload(
                    client,
                    transport,
                    b"""
from pybricks.parameters import Color
from pybricks.tools import run_task
Color.CUSTOM = Color(123)
async def stuck():
    print('running')
    while True:
        pass
run_task(stuck())
""",
                    Format.PYTHON,
                )
                await checked(client, Start())
                async with asyncio.timeout(3):
                    while not (await checked(client, Status())).queued:
                        await asyncio.sleep(0.01)
                assert (await checked(client, Status())).state == 1
                assert await output(client) == b"running\r\n"
                await checked(client, Stop())
                assert (await checked(client, Status())).cleanup == 0
                await upload(
                    client,
                    transport,
                    b"""
from pybricks.parameters import Color
from pybricks.tools import run_task, wait
assert 'CUSTOM' not in Color
assert not run_task()
async def healthy():
    await wait(1)
run_task(healthy())
print('fresh library')
""",
                    Format.PYTHON,
                )
                await checked(client, Start())
                assert (await completed(client)).result == 0
                assert await output(client) == b"fresh library\r\n"
            finally:
                await checked(client, Stop())

    with native_console(ROOT / "build/python-smp-tests") as port:
        asyncio.run(exercise(port))
