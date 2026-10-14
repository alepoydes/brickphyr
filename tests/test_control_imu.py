# SPDX-License-Identifier: Apache-2.0
"""Sample-triggered native control through synthetic IMU, Python and SMP."""

import asyncio
import io
import struct
from pathlib import Path

import cbor2
from test_native import native_console
from test_python import completed
from test_python_hub import shell

from brickphyrdev import console, recording
from brickphyrdev.application import Format, Start, Status, Stop, checked, upload
from brickphyrdev.records import Recording
from brickphyrdev.telemetry import snapshot

BUILD = Path(__file__).resolve().parents[1] / "build/python-imu-tests"
OBSERVATION = struct.Struct("<qIIIi")


def bundle(source):
    return cbor2.dumps({
        "main.py": source.encode(),
        "controller.elf": (BUILD / "control_imu_probe.elf").read_bytes(),
    })


async def frames(client):
    log = io.StringIO()
    await recording.collect(client, log, "sample-control", {"records": 0, "gaps": 0})
    reader = Recording(io.StringIO(log.getvalue()))
    assert reader.schema == "fixture.control-imu/1"
    assert reader.control and reader.parameter_size == 8
    result = list(reader.frames(allow_incomplete=True))
    assert reader.gaps == 0
    return result


def test_sample_release_matches_imu_edge_and_python_revision():
    source = """import brickphyr, struct
from pybricks.tools import wait
def params(value):
    return struct.pack('<II', 0, value)
brickphyr.control_start(open('controller.elf', 'rb').read(), params(11))
for _ in range(2000):
    status = brickphyr.control_status()
    if status['sequence'] >= 2:
        assert status['period_us'] == 0
        assert brickphyr.control_update(params(22)) == 2
        break
    wait(1)
else:
    raise AssertionError('sample controller did not advance')
while True:
    wait(10)
"""

    async def exercise(device):
        async with console.connection(lambda _: None, port=device, management=True) as (port, client):
            try:
                await upload(client, port, bundle(source), Format.FILES)
                await checked(client, Start())
                state = await completed(client)
                assert state.result == state.fault == 0, state
                observed = await frames(client)
                assert len(observed) == 8
                assert [frame.control["sequence"] for frame in observed] == list(range(1, 9))
                assert {frame.control["parameter_revision"] for frame in observed} == {1, 2}
                previous_raw = None
                for frame in observed:
                    sample_us, raw_sequence, epoch, value, error = OBSERVATION.unpack(frame.data)
                    step = frame.control
                    assert error == 0 and epoch > 0
                    assert step["scheduled_us"] == sample_us > 0
                    assert step["started_us"] >= sample_us
                    assert frame.parameters == struct.pack("<II", 0, value)
                    assert value == {1: 11, 2: 22}[step["parameter_revision"]]
                    if previous_raw is None:
                        assert step["skipped"] == 0
                    else:
                        assert raw_sequence > previous_raw
                        assert step["skipped"] == raw_sequence - previous_raw - 1
                    previous_raw = raw_sequence
                assert any(frame.control["skipped"] > 0 for frame in observed[2:])
            finally:
                await checked(client, Stop())

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))


def test_stop_aborts_sample_wait_and_restarts_control_identity():
    source = """import brickphyr, struct
from pybricks.tools import wait
brickphyr.control_start(open('controller.elf', 'rb').read(), struct.pack('<II', MODE, 44))
while True:
    wait(10)
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await upload(client, port, bundle(source.replace("MODE", "3")), Format.FILES)
                await checked(client, Start())
                async with asyncio.timeout(2):
                    while (await checked(client, recording.Info())).next < 1:
                        await asyncio.sleep(0.005)
                await shell(port, captured, b"imu_error 2")
                # Exceed the ordinary 30 ms timeout while the probe waits with
                # its 30 s deadline: STOP must abort, not await that deadline.
                await asyncio.sleep(0.04)
                assert (await checked(client, Status())).state == 1
                async with asyncio.timeout(1):
                    await checked(client, Stop())
                stopped = await checked(client, Status())
                assert stopped.state != 1 and stopped.fault == stopped.cleanup == 0
                await shell(port, captured, b"imu_error 0")
                await upload(client, port, bundle(source.replace("MODE", "0")), Format.FILES)
                await checked(client, Start())
                state = await completed(client)
                assert state.result == state.fault == state.cleanup == 0, state
                observed = await frames(client)
                assert len(observed) == 8
                assert observed[0].control["sequence"] == 1
                assert observed[0].control["skipped"] == 0
            finally:
                await checked(client, Stop())

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))


def test_sample_wait_reports_io_error_and_stale_timeout_without_new_step():
    source = """import brickphyr, struct
from pybricks.tools import wait
brickphyr.control_start(open('controller.elf', 'rb').read(), struct.pack('<II', MODE, 33))
while True:
    wait(10)
"""

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await upload(client, port, bundle(source.replace("MODE", "1")), Format.FILES)
                await checked(client, Start())
                async with asyncio.timeout(2):
                    while (await checked(client, recording.Info())).next < 1:
                        await asyncio.sleep(0.005)
                await shell(port, captured, b"imu_error 1")
                state = await completed(client)
                assert state.result == -5 and state.fault == 0, state
                observed = await frames(client)
                assert len(observed) >= 2
                before, failed = observed[-2:]
                assert OBSERVATION.unpack(before.data)[-1] == 0
                assert OBSERVATION.unpack(failed.data)[-1] == -5
                assert failed.control == before.control
                await checked(client, Stop())

                await shell(port, captured, b"imu_error 0")
                async with asyncio.timeout(2):
                    while (await snapshot(client, "imu.calibrated")).error:
                        await asyncio.sleep(0.005)
                await shell(port, captured, b"imu_error 2")
                await asyncio.sleep(0.02)  # Four IMU periods is under 5 ms.
                assert (await snapshot(client, "imu.calibrated")).error == -116
                await upload(client, port, bundle(source.replace("MODE", "2")), Format.FILES)
                await checked(client, Start())
                state = await completed(client)
                assert state.result == -116 and state.fault == 0, state
                assert await frames(client) == []
            finally:
                await checked(client, Stop())

    with native_console(BUILD, virtual_clock=True) as device:
        asyncio.run(exercise(device))
