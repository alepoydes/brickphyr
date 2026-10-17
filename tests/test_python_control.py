# SPDX-License-Identifier: Apache-2.0
"""Independent native controller through the Python application and SMP paths."""

import asyncio
import re
import struct
from pathlib import Path

import cbor2
import pytest

from brickphyrdev import console, recording
from brickphyrdev.application import Format, Input, Start, Status, Stop, checked, connection, upload
from test_native import native_console
from test_python import completed, output


BUILD = Path(__file__).resolve().parents[1] / "build/python-smp-tests"
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
                assert status.control_stack >= 2048
                assert 0 < status.control_unused < status.control_stack
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


@pytest.mark.parametrize(("compact", "parameter_size"), [(True, 8), (False, 8),
                                                          (False, 176), (False, 256)])
def test_segmented_control_record_includes_applied_parameters(compact, parameter_size):
    import io

    from brickphyrdev.records import Recording

    source = """import brickphyr, struct
from pybricks.tools import wait
brickphyr.control_start(open('controller.elf', 'rb').read(), struct.pack('<II', 10, 7) + bytes(PAD_BYTES))
input()
assert brickphyr.control_update(struct.pack('<II', 10, 8) + bytes(PAD_BYTES)) == 2
while brickphyr.control_status()['applied_revision'] < 2:
    wait(1)
print('updated')
while brickphyr.record_info()['next'] < 10:
    wait(1)
""".replace("PAD_BYTES", str(parameter_size - 8))

    async def exercise(port):
        async with connection(port=port) as (client, transport):
            try:
                payload = cbor2.dumps({
                    "main.py": source.encode(),
                    "controller.elf": (BUILD / (
                        "record_frame_segments_params_probe.elf" if compact else
                        "record_frame_segments_params_big_probe.elf" if parameter_size > 8 else
                        "record_frame_segments_params_raw_probe.elf"
                    )).read_bytes(),
                })
                await upload(client, transport, payload, Format.FILES)
                await checked(client, Start())
                async with asyncio.timeout(3):
                    while (info := await checked(client, recording.Info())).next < 1:
                        await asyncio.sleep(0.01)
                assert info.segments and info.compact == compact
                assert info.parameter_size == parameter_size
                assert info.inline_parameters == (parameter_size <= 176)
                await checked(client, Input(data=b"update\n"))
                text = bytearray()
                async with asyncio.timeout(3):
                    while b"updated" not in text:
                        text.extend(await output(client))
                        await asyncio.sleep(0.01)

                class NoAcknowledgement:
                    def __init__(self):
                        self.lock = asyncio.Lock()
                        self.rows = []
                        self.parameters = 0

                    async def request(self, request):
                        assert not isinstance(request, (recording.Ack, recording.CompactAck))
                        self.parameters += isinstance(request, recording.Parameters)
                        async with self.lock:
                            reply = await client.request(request)
                            if isinstance(request, recording.Compact) and reply.rows:
                                self.rows.append(reply.rows)
                            return reply

                transfer = NoAcknowledgement()
                log = io.BytesIO()
                with recording.session(log) as (session, counts):
                    await asyncio.wait_for(recording.collect(
                        transfer, log, session, counts, wait=True
                    ), 5)
                log.seek(0)
                frames = list(Recording(log).frames(allow_gaps=True))
                if compact:
                    assert transfer.rows[0] == 1
                else:
                    assert not transfer.rows
                if parameter_size > 176:
                    assert 0 < transfer.parameters <= 10
                else:
                    assert transfer.parameters == 0
                assert [frame.index for frame in frames] == list(range(10)), (
                    await checked(client, Status()), await output(client)
                )
                assert {frame.control["parameter_revision"] for frame in frames} == {1, 2}
                assert all(struct.unpack("<II", frame.parameters[:8]) == (
                    10, 7 if frame.control["parameter_revision"] == 1 else 8
                ) for frame in frames)
                assert all(frame.parameters[8:] == bytes(parameter_size - 8)
                           for frame in frames)
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
    source = """import brickphyr, errno, struct
from pybricks.tools import wait
try:
    brickphyr.record_info()
    raise AssertionError('recording existed before begin')
except OSError as error:
    assert error.args[0] == errno.ENODATA
def params(value):
    return struct.pack('<4I', 0, value, (~value) & 0xffffffff, 0)
brickphyr.control_start(open('controller.elf', 'rb').read(), params(7))
while brickphyr.control_status()['sequence'] < 20:
    wait(1)
info = brickphyr.record_info()
assert info['control'] and info['schema'] == 'fixture.control/1'
assert info['size'] == 48 and info['parameter_size'] == 16 and info['first'] > 0
storage = bytearray(64)
storage[:] = b'x' * len(storage)
data, parameters = memoryview(storage)[:48], memoryview(storage)[48:]
generation = info['generation']
def rejected(generation, index, data, parameters, expected):
    before = bytes(data), bytes(parameters)
    try:
        brickphyr.record_readinto(generation, index, data, parameters)
        raise AssertionError('invalid record accepted')
    except OSError as error:
        assert error.args[0] == expected
        assert (bytes(data), bytes(parameters)) == before
index = info['next'] - 1
rejected(generation, index, bytearray(47), parameters, errno.EMSGSIZE)
rejected(generation, index, data, bytearray(15), errno.EMSGSIZE)
rejected(generation, index, data, data, errno.EINVAL)
rejected(generation + 1, index, data, parameters, errno.ESTALE)
rejected(generation, 0, data, parameters, errno.EOVERFLOW)
rejected(generation, 1 << 63, data, parameters, errno.EINVAL)
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
    copied = bytearray(64)
    copied[:48] = data
    memoryview(copied)[48:] = parameters
    assert copied == storage
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


async def heap_allocated(transport, shell):
    shell.clear()
    await transport.write(b"kernel heap\r")
    async with asyncio.timeout(2):
        while not shell.endswith(b"brickphyr> "):
            await asyncio.sleep(0.005)
    match = re.search(rb"\r\nallocated:\s+(\d+)", shell)
    assert match, bytes(shell)
    return int(match[1])


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
            baseline = await heap_allocated(transport, shell)
            try:
                await upload(client, transport, bundle(fault_source), Format.FILES)
                await checked(client, Start())
                fault = await completed(client, state=4)
                assert fault.fault != 0, fault
                await checked(client, Stop())
                await checked(client, recording.Clear())
                assert await heap_allocated(transport, shell) == baseline

                await upload(client, transport, bundle(spin_source), Format.FILES)
                await checked(client, Start())
                async with asyncio.timeout(2):
                    while (await checked(client, recording.Info())).next < 1:
                        assert (await checked(client, Status())).state == 1
                        await asyncio.sleep(0.01)
                await checked(client, Stop())
                stopped = await checked(client, Status())
                assert stopped.state == 3 and stopped.cleanup == 0, stopped
                assert 0 < stopped.control_unused < stopped.control_stack
                await checked(client, recording.Clear())
                assert await heap_allocated(transport, shell) == baseline

                await upload(client, transport, b"print('Python after controller')\n", Format.PYTHON)
                await checked(client, Start())
                returned = await completed(client)
                assert returned.result == 0 and b"Python after controller" in await output(client)
                await checked(client, Stop())
                assert await heap_allocated(transport, shell) == baseline
            finally:
                await checked(client, Stop())

    with native_console(BUILD) as port:
        asyncio.run(exercise(port))


@pytest.mark.parametrize(('build', 'modes'), [
    pytest.param(BUILD, (0, 1, 2, 0), id='full'),
    pytest.param(BUILD.parent / 'native-smp-tests', (0,), id='native_only'),
])
def test_native_background_progress_isolation_and_reclamation(build, modes):
    async def exercise(port):
        shell = bytearray()
        async with console.connection(shell.extend, management=True, port=port) as (transport, client):
            baseline = await heap_allocated(transport, shell)
            try:
                # Normal return, cross-domain fault, forced stop, then reuse.
                for mode in modes:
                    await upload(client, transport, (build / f"background_{mode}.elf").read_bytes())
                    await checked(client, Start())
                    if mode == 2:
                        async with asyncio.timeout(2):
                            while (await checked(client, recording.Info())).next < 6:
                                assert (await checked(client, Status())).state == 1
                                await asyncio.sleep(0.005)
                        await checked(client, Stop())
                        state = await checked(client, Status())
                        assert state.state == 3 and state.cleanup == 0, state
                    else:
                        state = await completed(client, state=4 if mode == 1 else 2)
                        if mode == 1:
                            assert state.fault, state
                        else:
                            assert state.result == state.fault == state.cleanup == 0, (state, await output(client))
                            assert b"native background complete" in await output(client)
                            assert any(row[4] == 2 for row in await records(client))
                    assert 0 < state.control_unused < state.control_stack, state
                    await checked(client, Stop())
                    await checked(client, recording.Clear())
                    assert await heap_allocated(transport, shell) == baseline
            finally:
                await checked(client, Stop())

    with native_console(build) as port:
        asyncio.run(exercise(port))
