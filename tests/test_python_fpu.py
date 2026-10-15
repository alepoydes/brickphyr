# SPDX-License-Identifier: Apache-2.0
"""Hard-float dynamic imports and admission through the protected ARM runtime."""

import asyncio
from pathlib import Path

import cbor2
import pytest
from test_native import native_console
from test_python import completed, output

from brickphyrdev.application import Format, Start, checked, connection, upload
from brickphyrdev.smp import Rejected

BUILD = Path(__file__).resolve().parents[1] / "build/python-smp-tests"


def test_hard_float_import_and_admission():
    async def exercise(port):
        module = (BUILD / "python-fixtures/native_module.mpy").read_bytes()
        source = b"""import gc, math, native_module
assert native_module.add(30) == 42
values = [math.sqrt(2.0), math.sqrt(3.0)]
try:
    math.sqrt(-1.0)
except ValueError:
    gc.collect()
assert abs(values[0] * values[0] + values[1] * values[1] - 5.0) < 0.00001
print('float import and exception recovered')
"""
        async with connection(port=port) as (client, transport):
            await upload(client, transport, cbor2.dumps({
                "main.py": source, "native_module.mpy": module,
            }), Format.FILES)
            await checked(client, Start())
            status = await completed(client)
            text = await output(client)
            assert status.result == 0 and status.fault == 0, (status, text)
            assert b"native C 42 1" in text and b"float import and exception recovered" in text
            await upload(client, transport, (BUILD / "python-fixtures/native.mpy").read_bytes(), Format.MPY)
            await checked(client, Start())
            status = await completed(client)
            text = await output(client)
            assert status.result == 0 and b"native and viper 42 4950" in text, (status, text)
            # A soft-ABI C module must be rejected before any module initialization,
            # both as the top-level image and inside an importable file package.
            incompatible = bytearray(module)
            incompatible[2] = (5 << 2) | (module[2] & 3)  # MP_NATIVE_ARCH_ARMV7M
            for image, kind in (
                (bytes(incompatible), Format.MPY),
                (cbor2.dumps({"main.py": source, "native_module.mpy": bytes(incompatible)}), Format.FILES),
                # Old hard-float native modules have a shorter exception frame.
                (module[:1] + bytes([6, module[2] | 3]) + module[3:], Format.MPY),
            ):
                with pytest.raises(Rejected) as rejection:
                    await upload(client, transport, image, kind)
                assert rejection.value.code == 134  # Zephyr ENOTSUP

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))
