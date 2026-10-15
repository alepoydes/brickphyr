# SPDX-License-Identifier: Apache-2.0
"""Live memory accounting and selected Python stacks through ARM/SMP."""

import asyncio

import cbor2
import pytest
from test_native import native_console
from test_python import output
from test_python_files import BUILD

from brickphyrdev import application as app
from brickphyrdev import budget, recording


def test_native_budget_independent_of_python():
    """Map a real 64 KiB BSS, probe its pages and use an 8 KiB stack beside idle Python."""
    image = (BUILD / "capacity_probe.elf").read_bytes()

    async def exercise(port):
        async with app.connection(port=port) as (client, transport):
            baseline = await app.checked(client, app.Memory())
            assert baseline.stack_max == 16384 and baseline.stack_default == 12288
            await app.upload(client, transport, image, app.Format.NATIVE)
            estimate = await app.checked(client, budget.ImageBudget())
            assert estimate.native > 65536 and estimate.stack == 8192 and estimate.arena == 0
            await app.checked(client, app.Start())
            text = bytearray()
            async with asyncio.timeout(3):
                while True:
                    text.extend(await output(client))
                    state = await app.checked(client, app.Status())
                    if state.state != 1:
                        break
                    await asyncio.sleep(.005)
            text.extend(await output(client))
            assert state.state == 2 and state.result == state.fault == state.cleanup == 0, state
            assert state.stack == 8192 and b"CAPACITY_OK" in text
            assert (await app.checked(client, app.Memory())).heap_used == baseline.heap_used

            # The native limit must not enlarge the arena or default Python stack.
            await app.upload(client, transport, b"input()", app.Format.PYTHON)
            estimate = await app.checked(client, budget.ImageBudget())
            assert estimate.native == 0 and estimate.arena == 65536 and estimate.stack == 12288
            await app.checked(client, app.Start())
            live = await app.checked(client, app.Memory())
            assert live.active and live.python_arena == 65536
            await app.checked(client, app.Stop())
            assert (await app.checked(client, app.Memory())).heap_used == baseline.heap_used

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_live_memory_and_python_stack():
    resource = "c" * 124 + ".elf"  # Longest valid package path, including its suffix.
    image = cbor2.dumps({
        "main.py": b"""import gc, brickphyr, struct, native_module
assert native_module.add(30) == 42
brickphyr.control_start('controller.elf', struct.pack('<4I', 0, 7, 0xfffffff8, 0))
gc.collect()
print('ready')
input()
""".replace(b"controller.elf", resource.encode()),
        resource: (BUILD / "control_probe.elf").read_bytes(),
        "native_module.mpy": (BUILD / "python-fixtures/native_module.mpy").read_bytes(),
    })

    async def exercise(port):
        async with app.connection(port=port) as (client, transport):
            baseline = await app.checked(client, app.Memory())
            for stored, stack in ((False, 4096), (True, 8192)):
                if stored:
                    await app.install(client, image, "memory", app.Format.FILES)
                else:
                    await app.upload(client, transport, image, app.Format.FILES)
                before = await app.checked(client, app.Memory())
                primary = await app.checked(client, budget.ImageBudget(name="memory" if stored else "", stack=stack))
                control = await app.checked(client, budget.ImageBudget(
                    name="memory" if stored else "", resource=resource, control=True, parameters=16))
                recorder = await app.checked(client, budget.RecordBudget(size=48, capacity=16, control=True, parameters=16))
                after = await app.checked(client, app.Memory())
                assert not after.active and after.heap_used == before.heap_used
                assert after.staging == before.staging
                await app.checked(client, app.RunStoredWithStack(name="memory", stack=stack) if stored else app.StartWithStack(stack=stack))
                text = bytearray()
                async with asyncio.timeout(3):
                    while b"ready" not in text:
                        text.extend(await output(client))
                        assert (await app.checked(client, app.Status())).state == 1, text
                        await asyncio.sleep(.005)
                async with asyncio.timeout(2):
                    while (live := await app.checked(client, app.Memory())).gc_free < 0:
                        await asyncio.sleep(.005)
                assert live.active and live.python_arena == 65536
                assert live.code_used > 0 and live.code >= live.code_used
                assert live.control_native > 0 and live.records > 0
                assert primary.arena == live.python_arena and primary.source_ram == live.source_ram
                assert primary.source_flash == live.source_flash and primary.code_limit == live.code
                assert control.native >= live.control_native and recorder.bytes == live.records
                assert live.gc_free > 40000 and 0 <= live.gc_age_ms < 1000
                assert live.source_ram == (0 if stored else len(image))
                assert live.source_flash == (len(image) if stored else 0)
                assert live.staging == 0
                snapshot = await app.memory_snapshot(client)
                primary = next(s for s in snapshot["stacks"] if s["kind"] == "primary")
                control = next(s for s in snapshot["stacks"] if s["kind"] == "controller")
                assert primary["requested"] == primary["user"] == stack
                assert 0 < primary["unused"] < stack
                assert control["requested"] == control["user"] == 2048
                assert primary["privileged"] > 0 and primary["priv_unused"] < 0
                if stored:
                    generation = (await app.checked(client, recording.Info())).generation
                    with pytest.raises(RuntimeError, match="RecordBudget rejected"):
                        await app.checked(client, budget.RecordBudget(size=1024, capacity=32768))
                    assert (await app.checked(client, recording.Info())).generation == generation
                    with pytest.raises(RuntimeError, match="RunStoredWithStack rejected"):
                        await app.checked(client, app.RunStoredWithStack(name="memory", stack=1))
                    assert (await app.checked(client, app.Status())).state == 1
                await app.checked(client, app.Stop())
                released = await app.checked(client, app.Memory())
                assert not released.active and released.python_arena == released.code == 0
                assert released.source_ram == released.source_flash == released.control_native == 0
                assert released.records > 0  # Retained records have their own lifetime.
                await app.checked(client, recording.Clear())
                assert (await app.checked(client, app.Memory())).heap_used == baseline.heap_used


    with native_console(BUILD) as port:
        asyncio.run(exercise(port))
