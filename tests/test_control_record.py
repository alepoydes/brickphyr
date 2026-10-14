# SPDX-License-Identifier: Apache-2.0
"""Correlate SDK commands and recorded driver results through protected ARM/SMP."""

import asyncio
import io
import json
from pathlib import Path

from test_native import native_console
from test_python import completed
from test_python_hub import shell

from brickphyrdev import console, recording
from brickphyrdev.application import Start, Stop, checked, upload
from brickphyrdev.telemetry import snapshot


def test_group_example_records_input_command_applied_output_and_expiry():
    build = Path(__file__).resolve().parents[1] / "build/native-smp-tests"
    examples = build.with_name("native-smp-examples")

    async def exercise(device):
        captured = bytearray()
        async with console.connection(captured.extend, port=device, management=True) as (port, client):
            try:
                await shell(port, captured, b"pup_peer encoded_motor")
                await shell(port, captured, b"pup_peer B encoded_motor")
                # Only wait for physical negotiation; no motor constructor or
                # diagnostic read sets up a prerequisite for the native program.
                async with asyncio.timeout(3):
                    for name in ("port.A", "port.B"):
                        while (await snapshot(client, name)).values["phase"] != 7:
                            await asyncio.sleep(0.005)

                for example, coast in (("control_group", False), ("control_group_coast", True)):
                    await upload(client, port, (examples / f"{example}.elf").read_bytes())
                    await checked(client, Start())
                    state = await completed(client)
                    assert state.result == state.fault == 0, state
                    output = io.StringIO()
                    counts = {"records": 0, "gaps": 0}
                    await recording.collect(client, output, "arm-fixture", counts)
                    events = [json.loads(line) for line in output.getvalue().splitlines()]
                    records = [event for event in events if event["type"] == "record"]
                    rows = [event["values"] for event in records]
                    assert [row["phase"] for row in rows] == ([0, 2] if coast else [0, 1, 2])
                    assert counts == {"records": len(rows), "gaps": 0}
                    assert len({row["data"][:176 * 2] for row in records}) == 1
                    for row in rows:
                        assert row["issue_error"] == row["group_read_error"] == 0
                        assert row["port_mask"] == row["group"]["port_mask"] == 3
                        assert row["coast_mask"] == (3 if coast else 0)
                        assert row["command_duty"] == ([0, 0] if coast else [200, -200])
                        assert row["command_generation"] == row["input"]["port_generation"]
                        assert row["input"]["errors"][1:3] == [0, 0]
                        assert row["request"] == row["group"]["request"] != 0
                        assert row["deadline_ms"] == row["group"]["deadline_ms"]
                        assert row["command_ms"] <= row["group"]["requested_ms"] <= row["observed_ms"]

                    final = rows[-1]
                    assert not final["group"]["active"] and not final["group"]["pending"]
                    assert final["group"]["applied_mask"] == 3
                    # Wire errors use Zephyr/Picolibc numbers, not host errno.
                    assert final["group"]["error"] == (0 if coast else -116)
                    assert final["group"]["cleanup_error"] == 0
                    if not coast:
                        assert final["group"]["completed_ms"] >= final["deadline_ms"]
                        applied = rows[1]
                        assert applied["group"]["active"] and not applied["group"]["pending"]
                        assert applied["group"]["error"] == 0
                        assert [p["duty_permille"] for p in applied["ports"]] == [200, -200]
                        for p in applied["ports"]:
                            assert p["mode"] == 1 and p["valid"] and not p["pending"]
                            assert p["completed"] == p["group_request"] == p["request"]
                            assert p["applied_ms"] >= applied["group"]["requested_ms"]
                    for p in final["ports"]:
                        assert p["mode"] == p["duty_permille"] == p["error"] == p["cleanup_error"] == 0
                        assert p["valid"] and not p["pending"]
                        assert p["generation"] == p["group_generation"]
                        assert p["completed"] == p["request"]
                        assert p["completed"] >= p["group_request"]
                    for name in ("motor.A", "motor.B"):
                        sample = await snapshot(client, name)
                        assert sample.values["owner"] == sample.values["mode"] == 0

                # Input rejection is recorded too, and cannot become an output.
                await shell(port, captured, b"pup_peer B unplug")
                async with asyncio.timeout(2):
                    while (await snapshot(client, "port.B")).values["phase"] != 1:
                        await asyncio.sleep(0.005)
                await upload(client, port, (examples / "control_group.elf").read_bytes())
                await checked(client, Start())
                state = await completed(client)
                assert state.result == -19, state
                output = io.StringIO()
                counts = {"records": 0, "gaps": 0}
                await recording.collect(client, output, "rejected", counts)
                events = [json.loads(line) for line in output.getvalue().splitlines()]
                rows = [event["values"] for event in events if event["type"] == "record"]
                assert len(rows) == 1 and rows[0]["request"] == 0
                assert rows[0]["issue_error"] == rows[0]["input"]["errors"][2] == -19
                sample = await snapshot(client, "motor.A")
                assert sample.values["owner"] == sample.values["mode"] == 0
            finally:
                await checked(client, Stop())

    with native_console(build) as device:
        asyncio.run(exercise(device))
