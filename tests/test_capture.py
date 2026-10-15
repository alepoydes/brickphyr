# SPDX-License-Identifier: Apache-2.0
"""Built-in capture through ARM Shell/SMP and real lifecycle; synthetic devices."""

import asyncio
import io
from pathlib import Path

import pytest
from test_native import native_console
from test_python_hub import shell

from brickphyrdev import application, console, recording, telemetry
from brickphyrdev.records import Recording
from brickphyrdev.smp import checked


def test_capture_lifetime_and_collection():
    build = Path(__file__).resolve().parents[1] / "build/native-smp-tests"

    async def exercise(port_name):
        output = bytearray()
        async with console.connection(output.extend, port=port_name, management=True) as (port, client):
            await application.upload(client, port, (build / "hello.elf").read_bytes(),
                                     application.Format.NATIVE)
            response = await shell(port, output, b"capture prepare 10 300 battery encoder.A motor.A")
            assert b"Capture: prepared" in response, response
            # No diagnostic reads or warm-up commands establish prerequisites.
            response = await shell(port, output, b"capture start")
            assert b"failed" not in response, response
            initial = await checked(client, recording.Info())
            with pytest.raises(RuntimeError, match="16"):
                await checked(client, application.Start())
            with pytest.raises(RuntimeError, match="16"):
                await checked(client, recording.Clear())
            await checked(client, application.Stop())
            power = await telemetry.snapshot(client, "power")
            assert power.values["recording"] == 1
            # Closing and opening management must retain the producer and data.
        async with console.connection(output.extend, port=port_name, management=True) as (port, client):
            log = io.BytesIO()
            with recording.session(log) as (session, counts):
                await recording.collect(client, log, session, counts)
            log.seek(0)
            data = Recording(log)
            frames = list(data.frames())
            assert data.outcome["reason"] == "duration"
            assert "fault" not in data.outcome  # Previous application's result is unrelated.
            assert frames and frames[0].index == 0
            assert data.session["recording"]["generation"] == initial.generation
            previous = -1
            for frame in frames:
                value = frame.values
                assert previous < value["scheduled_us"] <= value["started_us"] <= value["completed_us"]
                previous = value["scheduled_us"]
                assert value["sources"]["motor.A"]["duty_permille"] == 0
            response = await shell(port, output, b"capture start")
            assert b"Capture failed: -16" in response  # Retained data needs explicit clear.
            assert (await checked(client, recording.Info())).generation == initial.generation
            await shell(port, output, b"capture clear")
            assert (await checked(client, recording.Info())).size == 0
            assert (await telemetry.snapshot(client, "power")).values["recording"] == 0

            # A full buffer freezes its beginning; the ordinary app poll keeps
            # running throughout. Empty/unavailable encoders are data errors.
            await shell(port, output, b"capture prepare 1 full all")
            await shell(port, output, b"capture start")
            log = io.BytesIO()
            with recording.session(log) as (session, counts):
                await recording.collect(client, log, session, counts)
            log.seek(0)
            data = Recording(log)
            full = list(data.frames())
            assert data.outcome["reason"] == "full"
            assert full[0].index == 0 and len(full) == data.session["recording"]["capacity"]
            await shell(port, output, b"capture clear")

            await shell(port, output, b"capture prepare 10 full battery")
            await shell(port, output, b"capture start")
            await shell(port, output, b"capture stop")
            cancelled = await checked(client, recording.Info())
            assert not cancelled.active and cancelled.reason == 5
            await shell(port, output, b"capture clear")
            # Releasing capture restores application admission, including an
            # uploaded image rejected while the recorder belonged to the bench.
            await checked(client, application.Start())
            async with asyncio.timeout(2):
                while (state := await checked(client, application.Status())).state == 1:
                    await asyncio.sleep(0)
            assert (state.result, state.fault, state.cleanup) == (0, 0, 0)

    with native_console(build) as port:
        asyncio.run(exercise(port))
