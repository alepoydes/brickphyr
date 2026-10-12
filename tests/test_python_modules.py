# SPDX-License-Identifier: Apache-2.0
"""Selected upstream module cases through the protected application lifecycle."""

import asyncio
import subprocess
import sys
from pathlib import Path

import pytest
from test_native import native_console
from test_python import completed, output

from brickphyrdev.application import Format, Start, checked, connection, upload

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/python-smp-tests"
UPSTREAM = ROOT.parent / "pybricks-micropython/micropython/tests"


def execute(source):
    async def exercise(port):
        async with connection(port=port) as (client, transport):
            await upload(client, transport, source, Format.PYTHON)
            await checked(client, Start())
            status = await completed(client)
            text = await output(client)
            assert status.result == 0 and status.dropped == 0, (status, text)
            return text.replace(b"\r\n", b"\n")

    with native_console(BUILD) as port:
        return asyncio.run(exercise(port))


@pytest.mark.parametrize("name", [
    "basics/memoryview1.py",
    "basics/class_reverse_op.py",
    "basics/struct1.py",
    "basics/io_iobase.py",
    "basics/gc1.py",
    "extmod/json_load.py",
    "extmod/random_extra.py",
    "micropython/heap_lock.py",
])
def test_upstream_module_case(name):
    # Execute the pinned dependency's cases unchanged. CPython supplies the
    # portable expected results; MicroPython-only cases keep their upstream oracle.
    path = UPSTREAM / name
    expected = path.with_suffix(".py.exp")
    if expected.exists():
        expected = expected.read_bytes()
    else:
        expected = subprocess.check_output([sys.executable, "-I", str(path)], timeout=3)
    assert b"SKIP" not in expected
    assert execute(path.read_bytes()) == expected


def test_runtime_diagnostics_and_numeric_module():
    result = execute(b"""
import math, micropython, gc, sys, io
assert abs(math.sin(math.pi / 6) - 0.5) < 1e-6
assert abs(math.atan2(1, 1) - math.pi / 4) < 1e-6
assert math.isinf(float('inf')) and math.isnan(float('nan'))
assert math.frexp(12.0) == (0.75, 4)
assert math.ldexp(0.75, 4) == 12.0
try:
    math.sqrt(-1)
    assert False
except ValueError:
    pass
assert 0 < micropython.stack_use() < 16384
assert gc.mem_free() > 0 and gc.mem_alloc() > 0
micropython.mem_info()
micropython.qstr_info()
micropython.opt_level(1)
assert micropython.opt_level() == 1
micropython.opt_level(0)
stream = io.StringIO()
try:
    raise ValueError('diagnostic exception')
except ValueError as error:
    sys.print_exception(error, stream)
assert 'ValueError: diagnostic exception' in stream.getvalue()
help('modules')
print('diagnostics passed')
""")
    assert b"stack:" in result and b"GC:" in result and b"qstr pool:" in result
    assert b"micropython" in result and b"select" in result
    assert result.endswith(b"diagnostics passed\n")
