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
            response = await shell(port, output,
                                   b"capture prepare 10 300 battery:voltage encoder.A:angle motor.A:duty")
            assert b"Capture: prepared" in response, response
            for selection in (b"battery:", b"battery:voltage,", b"battery:voltage,,current",
                              b"battery:voltage,voltage", b"battery:nope",
                              b"battery:voltage battery:current"):
                response = await shell(port, output, b"capture prepare 10 300 " + selection)
                assert b"Capture failed" in response, response
            # Rejected selections leave the original plan and its allocation
            # budget intact. Packed length checks also protect the shared layout.
            # No diagnostic reads or warm-up commands establish prerequisites.
            response = await shell(port, output, b"capture start")
            assert b"failed" not in response, response
            initial = await checked(client, recording.Info())
            assert (initial.record_schema, initial.size) == ("brickphyr.capture/2", 124)
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
                assert "requested_ms" not in value["sources"]["motor.A"]
                battery = value["sources"]["battery"]
                assert battery["voltage_mv"] == 7767 and battery["error"] == 0
                assert battery["sequence"] > 0 and battery["sample_ms"] <= battery["read_ms"]
                assert "current_ma" not in battery and "average_mv" not in battery
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
        asyncio.run(asyncio.wait_for(exercise(port), timeout=5))


def test_schedule_records_outputs_and_disconnect_cleanup():
    build = Path(__file__).resolve().parents[1] / "build/native-smp-tests"

    async def exercise(port_name):
        output = bytearray()
        async with console.connection(output.extend, port=port_name, management=True) as (port, client):
            async def command(text):
                response = await shell(port, output, text)
                assert b"Capture failed" not in response, response
                return response

            async def collect():
                log = io.BytesIO()
                with recording.session(log) as (session, counts):
                    await recording.collect(client, log, session, counts)
                log.seek(0)
                data = Recording(log)
                values = [frame.values for frame in data.frames()]
                assert data.complete and not data.gaps
                return data, values

            await command(b"pup_peer motor")
            await command(b"pup_peer B motor")
            async with asyncio.timeout(2):
                for name in ("port.A", "port.B"):
                    while True:
                        state = await telemetry.snapshot(client, name)
                        if not state.error and state.values["type_id"] == 2:
                            break
                        await asyncio.sleep(0)
            await command(b"capture prepare 10 600 battery:current motor.A:duty motor.B:duty")
            await command(b"capture at 100 150 A=250 B=-250")
            await command(b"capture at 350 100 A=-300 B=300")
            await command(b"capture check")
            for name in ("motor.A", "motor.B"):
                value = (await telemetry.snapshot(client, name)).values
                assert (value["owner"], value["duty_permille"]) == (0, 0)
            await command(b"capture start")
            data, values = await collect()
            assert data.outcome["reason"] == "duration", data.outcome
            assert data.session["recording"]["schema"] == "brickphyr.bench/3"
            samples = [value["sources"] for value in values if value["kind"] == "sample"]
            assert samples and all(value["battery"]["current_ma"] == 1825 for value in samples)
            assert all("voltage_mv" not in value["battery"] for value in samples)
            assert any(value["motor.A"]["duty_permille"] == 250 and
                       value["motor.B"]["duty_permille"] == -250 for value in samples)
            assert values[0]["kind"] == "begin" and values[-1]["kind"] == "end"
            end = values[-1]
            assert (end["error"], end["cleanup_error"], end["attempted_actions"]) == (0, 0, 2), end
            assert end["written_samples"] + end["missed"] == end["planned_samples"] == 60
            groups = [value for value in values if value["kind"] == "group"]
            assert [(value["action"], value["active"]) for value in groups] == [
                (1, True), (1, False), (2, True), (2, False),
            ], groups
            for action, offset, duties in ((1, 100_000, (250, -250)), (2, 350_000, (-300, 300))):
                applied, stopped = [value for value in groups if value["action"] == action]
                assert applied["scheduled_us"] == values[0]["origin_us"] + offset
                assert applied["submitted_us"] >= applied["scheduled_us"]
                assert applied["applied_mask"] == stopped["cleanup_mask"] == 3
                assert tuple(applied["ports"][p]["duty_permille"] for p in "AB") == duties
                for name in "AB":
                    assert stopped["ports"][name]["cleanup_us"] >= applied["ports"][name]["applied_us"]
            for name in ("motor.A", "motor.B"):
                value = (await telemetry.snapshot(client, name)).values
                assert (value["owner"], value["duty_permille"], value["pending"]) == (0, 0, 0)
            await command(b"capture clear")

            # Explicit stop joins cleanup after real synthetic output has been
            # applied, and retains its original command and later coast result.
            await command(b"capture prepare 20 1000 battery")
            await command(b"capture at 0 800 A=200 B=-200")
            await command(b"capture start")
            async with asyncio.timeout(1):
                while not (await telemetry.snapshot(client, "motor.A")).values["duty_permille"]:
                    await asyncio.sleep(0)
            await command(b"capture stop")
            data, values = await collect()
            assert data.outcome["reason"] == "cancelled", data.outcome
            assert values[-1]["attempted_actions"] == 1
            groups = [value for value in values if value["kind"] == "group"]
            assert groups[-1]["cleanup_mask"] == 3 and not groups[-1]["active"]
            for name in ("motor.A", "motor.B"):
                value = (await telemetry.snapshot(client, name)).values
                assert (value["owner"], value["duty_permille"], value["pending"]) == (0, 0, 0)
            await command(b"capture clear")

            # Explicit fixture boundary arrives before the first action. It
            # cancels this diagnostic, unlike the passive capture test above.
            await command(b"capture prepare 10 1000 battery")
            await command(b"capture at 500 100 A=250 B=-250")
            await command(b"capture start")
            await command(b"session_close")
            data, values = await collect()
            assert data.outcome["reason"] == "cancelled", data.outcome
            assert values[-1]["attempted_actions"] == 0
            assert all(value["kind"] != "group" for value in values)
            await command(b"capture clear")
            assert (await telemetry.snapshot(client, "power")).values["recording"] == 0

            # Mixed event sizes no longer reserve the largest slot for every
            # observation. Exercise a full capture beyond that old limit.
            await command(b"capture prepare 1 full battery")
            await command(b"capture at 0 0 A=coast B=coast")
            await command(b"capture start")
            data, values = await collect()
            meta = data.session["recording"]
            assert meta["variable"] and meta["size"] * meta["capacity"] > 32768
            assert data.outcome["reason"] == "full"
            assert values[-1]["error"] == values[-1]["cleanup_error"] == 0
            assert values[-1]["written_samples"] + values[-1]["missed"] == values[-1]["planned_samples"]
            await command(b"capture clear")

    with native_console(build) as port:
        asyncio.run(asyncio.wait_for(exercise(port), timeout=5))
