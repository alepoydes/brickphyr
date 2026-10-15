# SPDX-License-Identifier: Apache-2.0
"""Independent native controller through the Python application and SMP paths."""

import asyncio
import re
import struct
from pathlib import Path

import cbor2

from brickphyrdev import console, recording
from brickphyrdev.application import Format, Start, Status, Stop, checked, connection, upload
from test_native import native_console
from test_python import completed, output


BUILD = Path(__file__).resolve().parents[1] / "build/python-fpu-tests"
RECORD = struct.Struct("<qqIIIIIfII")


def bundle(source):
    return cbor2.dumps(
        {
            "main.py": source.encode(),
            "controller.elf": (BUILD / "control_probe.elf").read_bytes(),
        }
    )


async def records(client):
    info = await checked(client, recording.Info())
    assert (info.size, info.capacity, info.record_schema) == (48, 16, "fixture.control/1")
    assert info.control and info.parameter_size == 16
    result = []
    for index in range(info.first, info.next):
        response = await checked(
            client, recording.Record(generation=info.generation, index=index, off=0)
        )
        assert len(response.data) == RECORD.size
        row = RECORD.unpack(response.data)
        assert response.step == response.data[:32]
        parameters = await checked(client, recording.Parameters(generation=info.generation, index=index))
        assert parameters.revision == row[4]
        _, value, inverse, _ = struct.unpack('<4I', parameters.data)
        assert value == row[6] and inverse == (~value & 0xffffffff)
        result.append(row)
    return result


def test_python_control_runs_during_gc_and_copies_updates():
    source = """import brickphyr, gc, struct
def params(value):
    return struct.pack('<4I', 0, value, (~value) & 0xffffffff, 0)
try:
    brickphyr.control_start(b'bad image', params(1))
    raise AssertionError('invalid controller accepted')
except OSError:
    pass
brickphyr.control_start(open('controller.elf', 'rb').read(), params(7))
def wait_for(revision, steps):
    for n in range(20000):
        state = brickphyr.control_status()
        if state['applied_revision'] == revision and state['sequence'] >= steps:
            return state
        bytearray(64)
        value = n * 0.125 + 0.5
        assert value >= 0.5
        if n % 128 == 0:
            gc.collect()
    raise AssertionError('controller did not advance')
first = wait_for(1, 18)
assert first['period_us'] == 10000 and first['stack_bytes'] >= 2048
assert first['loader_bytes'] > 0 and first['loaded_bytes'] > 0
assert brickphyr.control_update(params(0x12345678)) == 2
second = wait_for(2, first['sequence'] + 1)
assert brickphyr.control_update(params(0x87654321)) == 3
third = wait_for(3, second['sequence'] + 1)
wait_for(3, third['sequence'] + 3)
print('controller revisions and GC passed')
"""

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                await upload(client, transport, bundle(source), Format.FILES)
                await checked(client, Start())
                status = await completed(client)
                text = await output(client)
                assert status.result == 0 and b"controller revisions and GC passed" in text, (
                    status,
                    text,
                )
                observed = await records(client)
                assert {row[4] for row in observed} >= {2, 3}, observed
                for row in observed:
                    scheduled, started, sequence, skipped, revision, size, value, total, _, reserved = row
                    assert started >= scheduled and sequence > 0 and size == 16
                    assert total == sequence * 0.25 and reserved == 0
                    if revision in (2, 3):
                        assert value == {2: 0x12345678, 3: 0x87654321}[revision]
                # Fill and recycle the bounded pool with distinct revisions,
                # after the first run exercised shared-revision ring overwrite.
                churn = """import brickphyr, struct
from pybricks.tools import wait
def params(value):
    return struct.pack('<4I', 0, value, (~value) & 0xffffffff, 0)
brickphyr.control_start(open('controller.elf', 'rb').read(), params(1))
for revision in range(1, 25):
    if revision > 1:
        assert brickphyr.control_update(params(revision)) == revision
    while brickphyr.control_status()['applied_revision'] < revision:
        wait(1)
"""
                await upload(client, transport, bundle(churn), Format.FILES)
                await checked(client, Start())
                status = await completed(client)
                assert status.result == status.fault == status.cleanup == 0, status
                observed = await records(client)
                # A valid revision may span several steps before Python updates.
                assert len(observed) == 16 and observed[-1][4] == 24 and observed[0][4] > 1
                assert all(row[4] == row[6] for row in observed)
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_python_control_overrun_then_return():
    source = """import brickphyr, struct
def params(mode):
    return struct.pack('<4I', mode, 11, (~11) & 0xffffffff, 0)
brickphyr.control_start(open('controller.elf', 'rb').read(), params(4))
for n in range(20000):
    if brickphyr.control_status()['sequence'] >= 5:
        break
else:
    raise AssertionError('controller did not resume after sleep')
assert brickphyr.control_update(params(1)) == 2
input()
"""

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                await upload(client, transport, bundle(source), Format.FILES)
                await checked(client, Start())
                status = await completed(client)
                assert status.result == 77, (status, await output(client))
                observed = await records(client)
                assert any(row[3] > 0 for row in observed), observed
                assert any(row[4] == 2 for row in observed), observed
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_python_reads_complete_control_records_and_detects_gaps():
    source = """import brickphyr, struct
from pybricks.tools import wait
try:
    brickphyr.record_info()
    raise AssertionError('recording existed before begin')
except OSError:
    pass
def params(value):
    return struct.pack('<4I', 0, value, (~value) & 0xffffffff, 0)
brickphyr.control_start(open('controller.elf', 'rb').read(), params(7))
while brickphyr.control_status()['sequence'] < 20:
    wait(1)
info = brickphyr.record_info()
assert info['control'] and info['schema'] == 'fixture.control/1'
assert info['size'] == 48 and info['parameter_size'] == 16 and info['first'] > 0
data, parameters = bytearray(48), bytearray(16)
generation = info['generation']
def rejected(generation, index, data, parameters):
    before = bytes(data), bytes(parameters)
    try:
        brickphyr.record_readinto(generation, index, data, parameters)
        raise AssertionError('invalid record accepted')
    except OSError:
        assert (bytes(data), bytes(parameters)) == before
index = info['next'] - 1
rejected(generation, index, bytearray(47), parameters)
rejected(generation, index, data, bytearray(15))
rejected(generation, index, data, data)
rejected(generation + 1, index, data, parameters)
rejected(generation, 0, data, parameters)
rejected(generation, 1 << 63, data, parameters)
for invalid_generation, invalid_index in ((generation + (1 << 32), index), (generation, 1 << 64), (generation, -1)):
    try:
        brickphyr.record_readinto(invalid_generation, invalid_index, data, parameters)
        raise AssertionError('cursor was truncated')
    except OverflowError:
        pass
for revision in range(1, 5):
    value = 7 if revision == 1 else revision * 31
    if revision > 1:
        assert brickphyr.control_update(params(value)) == revision
    while brickphyr.control_status()['applied_revision'] != revision:
        wait(1)
    index = brickphyr.record_info()['next'] - 1
    step = brickphyr.record_readinto(generation, index, data, parameters)
    row = struct.unpack('<qqIIIIIfII', data)
    assert step['control'] and step['size'] == len(data)
    assert (step['scheduled_us'], step['started_us'], step['sequence'], step['skipped'], step['applied_revision'], step['parameter_size']) == row[:6]
    assert row[4] == revision and row[6] == value and bytes(parameters) == params(value)
    assert row[7] == row[2] * .25
    # Returned storage is private; changing it cannot modify the controller's state.
    parameters[0] = 255
    again = brickphyr.record_readinto(generation, index, data, parameters)
    assert again == step and bytes(parameters) == params(value)
print('record snapshots passed')
"""

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                await upload(client, transport, bundle(source), Format.FILES)
                await checked(client, Start())
                status = await completed(client)
                text = await output(client)
                assert status.result == 0 and b"record snapshots passed" in text, (status, text)
                # Python consumes neither the host's cursor nor retained records.
                observed = await records(client)
                assert observed[-1][4] == 4
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


def test_python_control_fault_stop_and_heap_recovery():
    fault_source = """import brickphyr, struct
private = bytearray(16)
value = 23
parameters = struct.pack('<4I', 2, value, (~value) & 0xffffffff, id(private))
brickphyr.control_start(open('controller.elf', 'rb').read(), parameters)
input()
"""
    spin_source = """import brickphyr, struct
value = 29
brickphyr.control_start(open('controller.elf', 'rb').read(),
    struct.pack('<4I', 3, value, (~value) & 0xffffffff, 0))
input()
"""

    async def exercise(port):
        shell = bytearray()
        async with console.connection(shell.extend, management=True, port=port) as (
            transport,
            client,
        ):
            async def heap_allocated():
                shell.clear()
                await transport.write(b"kernel heap\r")
                async with asyncio.timeout(2):
                    while not shell.endswith(b"brickphyr> "):
                        await asyncio.sleep(0.005)
                match = re.search(rb"\r\nallocated:\s+(\d+)", shell)
                assert match, bytes(shell)
                return int(match[1])

            baseline = await heap_allocated()
            try:
                await upload(client, transport, bundle(fault_source), Format.FILES)
                await checked(client, Start())
                fault = await completed(client, state=4)
                assert fault.fault != 0, fault
                await checked(client, Stop())
                await checked(client, recording.Clear())
                assert await heap_allocated() == baseline

                await upload(client, transport, bundle(spin_source), Format.FILES)
                await checked(client, Start())
                async with asyncio.timeout(2):
                    while (await checked(client, recording.Info())).next < 1:
                        assert (await checked(client, Status())).state == 1
                        await asyncio.sleep(0.01)
                await checked(client, Stop())
                stopped = await checked(client, Status())
                assert stopped.state == 3 and stopped.cleanup == 0, stopped
                await checked(client, recording.Clear())
                assert await heap_allocated() == baseline

                await upload(client, transport, b"print('Python after controller')\n", Format.PYTHON)
                await checked(client, Start())
                returned = await completed(client)
                assert returned.result == 0 and b"Python after controller" in await output(client)
                await checked(client, Stop())
                assert await heap_allocated() == baseline
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))
