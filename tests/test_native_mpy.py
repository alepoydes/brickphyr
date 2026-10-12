# SPDX-License-Identifier: Apache-2.0
"""MicroPython's native producers through the protected ARM application path."""

import asyncio
from pathlib import Path

import pytest
from elftools.elf.elffile import ELFFile
from test_native import native_console
from test_python import completed, output

from brickphyrdev.application import Format, Start, Status, Stop, checked, connection, upload

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/python-smp-tests"
IMAGES = BUILD / "python-fixtures"


def test_native_mpy_execution():
    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                # Repeat with fresh VM/module BSS; include a GC between calls.
                for _ in range(3):
                    for image, format, expected in (
                        (
                            (ROOT / "tests/python/native.py").read_bytes(),
                            Format.PYTHON,
                            b"native and viper 42 4950",
                        ),
                        (
                            (IMAGES / "native.mpy").read_bytes(),
                            Format.MPY,
                            b"native and viper 42 4950",
                        ),
                        ((IMAGES / "native_module.mpy").read_bytes(), Format.MPY, b"native C 42 1"),
                    ):
                        await upload(client, transport, image, format)
                        await checked(client, Start())
                        status = await completed(client)
                        text = await output(client)
                        assert status.result == 0 and expected in text, (status, text)
                        assert 0 < status.code_used <= status.code and status.unused > 0
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_native_mpy_protection_and_stop():
    with (BUILD / "zephyr/zephyr.elf").open("rb") as artifact:
        symbols = ELFFile(artifact).get_section_by_name(".symtab")
        protected = symbols.get_symbol_by_name("application_domain")[0]["st_value"]

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                for image, format in (
                    ((IMAGES / "native_code_fault.mpy").read_bytes(), Format.MPY),
                    ((IMAGES / "native_heap_fault.mpy").read_bytes(), Format.MPY),
                    (
                        (
                            "import micropython\n@micropython.viper\ndef corrupt():\n"
                            f"    ptr32({protected})[0] = 0\ncorrupt()\n"
                        ).encode(),
                        Format.PYTHON,
                    ),
                ):
                    await upload(client, transport, image, format)
                    await checked(client, Start())
                    status = await completed(client, 4)
                    assert status.fault != 0, status
                    await upload(client, transport, b"print('after fault')\n", Format.PYTHON)
                    await checked(client, Start())
                    assert (await completed(client)).result == 0
                    assert b"after fault" in await output(client)
                await upload(
                    client,
                    transport,
                    b"import micropython\n@micropython.viper\n"
                    b"def forever():\n    while True: pass\nforever()\n",
                    Format.PYTHON,
                )
                await checked(client, Start())
                async with asyncio.timeout(3):
                    while not (status := await checked(client, Status())).code_used:
                        assert status.state == 1, (status, await output(client))
                        await asyncio.sleep(0.01)
                assert status.state == 1
                await checked(client, Stop())
                assert (await checked(client, Status())).state == 3
                await upload(
                    client, transport, (IMAGES / "native_module.mpy").read_bytes(), Format.MPY
                )
                await checked(client, Start())
                assert (await completed(client)).result == 0
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_native_mpy_rejection_and_exhaustion():
    image = (IMAGES / "native.mpy").read_bytes()

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                # MPY v6.3: x86, v7EM on M3, and a different native sub-version.
                for feature in (7, 27, image[2] ^ 1):
                    invalid = image[:2] + bytes([feature]) + image[3:]
                    with pytest.raises(RuntimeError, match="FormattedUpload rejected"):
                        await upload(client, transport, invalid, Format.MPY)
                await upload(client, transport, image[:-1], Format.MPY)
                await checked(client, Start())
                assert (await completed(client)).result == -8
                assert b"truncated .mpy" in await output(client)
                source = b"""import gc, micropython
try:
    while True:
        exec('@micropython.native\\ndef f(x): return x + 1')
        assert f(41) == 42
        gc.collect()
except MemoryError:
    print('native allocation exhausted')
"""
                await upload(client, transport, source, Format.PYTHON)
                await checked(client, Start())
                status = await completed(client)
                text = await output(client)
                assert status.result == 0 and b"native allocation exhausted" in text, (status, text)
                assert status.code - 512 < status.code_used <= status.code
                await upload(client, transport, image, Format.MPY)
                await checked(client, Start())
                assert (await completed(client)).result == 0
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))
