# SPDX-License-Identifier: Apache-2.0
"""Follow the real ARM application's output and detach without stopping it."""

import asyncio
import json
import signal
import subprocess
import sys
import time
from pathlib import Path

from test_native import native_console

from brickphyrdev import application as app
from brickphyrdev import console
from brickphyrdev.records import Recording


def test_follow_records_connection_failure(tmp_path):
    log = tmp_path / "failed.jsonl"
    result = subprocess.run(
        [str(Path(sys.executable).with_name("brickphyrdev")), "app", "--port",
         str(tmp_path / "missing-port"), "start", "--record", str(log)],
        capture_output=True, timeout=2,
    )
    assert result.returncode != 0
    event = json.loads(log.read_text())
    assert event["type"] == "end" and event["reason"] == "error"
    assert "missing-port" in event["error"]


def test_follow_output_and_recording_detach(tmp_path):
    build = Path(__file__).resolve().parents[1] / "build/native-smp-tests"
    with native_console(build) as port:
        cli = [str(Path(sys.executable).with_name("brickphyrdev")), "app", "--port", port]
        hello = subprocess.run(
            [*cli, "run", str(build.with_name("native-smp-examples") / "hello.elf"), "--follow"],
            capture_output=True, timeout=3,
        )
        assert hello.returncode == 0, hello.stderr
        assert hello.stdout == b"Hello from a native application.\n"
        assert b"result 0, fault 0, cleanup 0" in hello.stderr

        log = tmp_path / "records.jsonl"
        process = subprocess.Popen(
            [*cli, "run", str(build / "record_probe.elf"), "--record", str(log)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 3
            while True:
                assert process.poll() is None, process.communicate()
                lines = log.read_text().splitlines(keepends=True) if log.exists() else []
                events = [json.loads(line) for line in lines if line.endswith("\n")]
                if sum(event["type"] == "record" for event in events) >= 3:
                    break
                assert time.monotonic() < deadline, events
                time.sleep(.005)
            process.send_signal(signal.SIGINT)
            _, error = process.communicate(timeout=2)
            assert b"no application stop was requested" in error, error
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=2)

        async def inspect_and_stop():
            async with console.connection(lambda _: None, port=port, management=True) as (_, client):
                try:
                    status = await app.checked(client, app.Status())
                    assert status.state == 1 and status.fault == status.cleanup == 0
                finally:
                    await app.checked(client, app.Stop())

        asyncio.run(inspect_and_stop())
        with log.open() as source:
            reader = Recording(source)
            frames = list(reader.frames(allow_gaps=True, allow_incomplete=True))
            assert len(frames) >= 3 and frames[-1].index < 8
            assert all(frame.data == bytes((frame.index + i) % 256 for i in range(600))
                       for frame in frames)
            assert reader.end["reason"] == "cancelled" and not reader.complete
