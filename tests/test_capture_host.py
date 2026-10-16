# SPDX-License-Identifier: Apache-2.0
"""Scheduled capture cancellation through the real host transport dispatcher."""

import asyncio
import io
from pathlib import Path

from test_native import native_console
from test_python_hub import shell

from brickphyrdev import console, recording, telemetry
from brickphyrdev.records import Recording
from brickphyrdev.smp import checked


def test_scheduled_capture_cancels_on_host_disconnect():
    build = Path(__file__).resolve().parents[1] / "build/native-host-stacks"

    async def exercise(port_name):
        output = bytearray()
        async with console.connection(output.extend, port=port_name, management=True) as (port, client):
            await shell(port, output, b"pup_peer motor")
            async with asyncio.timeout(2):
                while True:
                    state = await telemetry.snapshot(client, "port.A")
                    if not state.error and state.values["type_id"] == 2:
                        break
                    await asyncio.sleep(0)

            for command in (
                b"capture prepare 20 2000 battery",
                b"capture at 1600 100 A=250",
                b"capture start",
            ):
                response = await shell(port, output, command)
                assert b"Capture failed" not in response, response

            response = await shell(port, output, b"detach")
            assert b"Synthetic DTR cycle scheduled" in response, response
            output.clear()
            # The fixture closes and reopens DTR 1.2 s after detach. No SMP
            # request is sent until the new Shell prompt proves re-admission.
            async with asyncio.timeout(2.5):
                while b"brickphyr> " not in output:
                    await asyncio.sleep(0.01)

            log = io.BytesIO()
            with recording.session(log) as (session, counts):
                await recording.collect(client, log, session, counts)
            log.seek(0)
            data = Recording(log)
            values = [frame.values for frame in data.frames()]
            assert data.complete and not data.gaps
            assert data.outcome["reason"] == "cancelled", data.outcome
            assert values[0]["kind"] == "begin" and values[-1]["kind"] == "end"
            assert values[-1]["attempted_actions"] == 0
            assert all(value["kind"] != "group" for value in values)
            motor = (await telemetry.snapshot(client, "motor.A")).values
            assert (motor["owner"], motor["duty_permille"], motor["pending"]) == (0, 0, 0)
            await shell(port, output, b"capture clear")
            assert (await checked(client, recording.Info())).size == 0

    with native_console(build) as port:
        asyncio.run(exercise(port))
