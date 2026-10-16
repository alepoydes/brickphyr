# SPDX-License-Identifier: Apache-2.0
"""Real Shell/SMP over PTYs; BLE notification/write delivery is explicitly synthetic."""

import asyncio
import fcntl
import json
import os
import selectors
import subprocess
import sys
import termios
import time
from pathlib import Path
from types import SimpleNamespace

import pytest
import serial
from smp import header, packet
from smpclient.requests.os_management import EchoWrite
from smpclient.transport import SMPTransportDisconnected
from test_management import firmware

from brickphyrdev import console, monitor, telemetry, terminal
from brickphyrdev.smp import checked


def test_console_framing_boundaries():
    output, replies = bytearray(), []
    decoder = console._Decoder(output.extend, replies.append)
    message = bytes(range(256))
    lines = list(packet.encode(message))
    ordinary = b"prompt> \x06!\x04?\x1b[A\r\n"
    wire = ordinary + ordinary.join(lines) + b"tail"
    for byte in wire:
        decoder.feed(bytes([byte]))
    assert replies == [message]
    assert output == ordinary * len(lines) + b"tail"
    with pytest.raises(ValueError, match="line exceeds"):
        decoder.feed(packet.START_DELIMITER + b"A" * 128)
    decoder = console._Decoder(lambda _: None, lambda _: None)
    with pytest.raises(ValueError, match="negotiated capacity"):
        decoder.feed(b"".join(packet.encode(b"x" * 600)))


def test_console_raw_smp_rejects_fragmented_oversize():
    async def exercise():
        transport = console.ConsoleTransport(lambda _: None, ble="fixture")
        transport.initialize(32)
        transport.pending = asyncio.get_running_loop().create_future()
        response_header = bytes.fromhex("0300002100000000")  # 33-byte body plus header
        transport._feed_smp(response_header[:3])
        assert not transport.closed.is_set()
        transport._feed_smp(response_header[3:])
        with pytest.raises(ValueError, match="negotiated capacity"):
            await transport.receive()
        assert transport.closed.is_set()

    asyncio.run(exercise())


def test_console_ble_connect_callback_lifetime(monkeypatch):
    # Synthetic Bleak callback delivery; no firmware, adapter, or radio is involved.
    state = {"recovers": True}
    radios = []

    class FakeRadio:
        def __init__(self, device, disconnected_callback, **kwargs):
            self.disconnected_callback = disconnected_callback
            self.is_connected = False
            self.rx = SimpleNamespace(max_write_without_response_size=20)
            self.smp = SimpleNamespace(max_write_without_response_size=20)
            self.services = SimpleNamespace(
                get_service=lambda uuid: uuid if uuid == console.SERVICE else None,
                get_characteristic=lambda uuid: {
                    console.RX: self.rx,
                    console.SMP_CHARACTERISTIC_UUID: self.smp,
                }.get(uuid),
            )
            self.writes = []
            self.notifications = []
            radios.append(self)

        async def connect(self):
            # A failed internal attempt can call back before Bleak retries.
            self.disconnected_callback(self)
            self.is_connected = state["recovers"]

        async def start_notify(self, characteristic, callback):
            self.notifications.append(characteristic)

        async def write_gatt_char(self, characteristic, data, response):
            self.writes.append((characteristic, data, response))

        async def disconnect(self):
            self.is_connected = False
            self.disconnected_callback(self)

    async def discover(**kwargs):
        return [SimpleNamespace(name="fixture", address="synthetic")]

    monkeypatch.setattr(console, "BleakClient", FakeRadio)
    monkeypatch.setattr(console.BleakScanner, "discover", discover)

    async def exercise():
        transport = console.ConsoleTransport(lambda _: None, ble="fixture", management=True)
        await transport.connect()
        radio = radios[-1]
        assert radio.is_connected and not transport.closed.is_set()
        await transport.write(b"shell input")
        assert radio.writes == [(radio.rx, b"shell input", True)]

        await transport.send(b"pending SMP")
        radio.is_connected = False
        radio.disconnected_callback(radio)
        assert transport.closed.is_set() and transport.pending.done()
        with pytest.raises(SMPTransportDisconnected, match="BLE console disconnected"):
            await transport.receive()
        await transport.disconnect()

        # A callback followed by no live connection is a genuine setup failure.
        state["recovers"] = False
        failed = console.ConsoleTransport(lambda _: None, ble="fixture", management=True)
        with pytest.raises(SMPTransportDisconnected, match="BLE console disconnected"):
            await failed.connect()
        assert failed.closed.is_set()
        assert radios[-1].notifications == []
        await failed.disconnect()

    asyncio.run(exercise())


def test_monitor_waits_for_output_boundary():
    for sequence in (
        b"\x1b[32m",
        b"\x1b(B",
        b"\x1b7",
        b"\x1b]0;title\x07",
        b"\x1b]0;title\x1b\\",
        b"\x1bPtext\x1b\\",
        "я".encode(),
    ):
        boundary = terminal._OutputBoundary()
        assert boundary.safe
        for byte in sequence[:-1]:
            boundary.feed(bytes([byte]))
            assert not boundary.safe
        boundary.feed(sequence[-1:])
        assert boundary.safe
    boundary = terminal._OutputBoundary()
    boundary.feed(b"\x1b[")
    boundary.feed(b"\x18")
    assert boundary.safe


@pytest.mark.parametrize("ble", [False, True])
def test_one_console_with_management(tmp_path, monkeypatch, ble):
    with firmware(tmp_path / "flash.bin") as port:
        instances = []

        class FakeRadio:
            # Only BLE delivery is simulated. The peer remains the real Zephyr
            # Shell/SMP fixture reached through its serial console.
            def __init__(self, device, disconnected_callback, **kwargs):
                self.disconnected = disconnected_callback
                self.rx = SimpleNamespace(max_write_without_response_size=20)
                self.smp = SimpleNamespace(max_write_without_response_size=20)
                self.services = SimpleNamespace(
                    get_service=lambda uuid: uuid == console.SERVICE,
                    get_characteristic=lambda uuid: (
                        self.rx
                        if uuid == console.RX
                        else self.smp
                        if uuid == console.SMP_CHARACTERISTIC_UUID
                        else None
                    ),
                )
                self.max_write = 0
                self.shell_callback = self.smp_callback = None
                self.decoder = console._Decoder(self.notify_shell, self.notify_smp)
                self.decoder.limit = 65535
                self.smp_request = bytearray()
                self.serial = None
                self.pause_writes = False
                instances.append(self)

            @property
            def is_connected(self):
                return self.serial is not None and self.serial.is_open

            async def connect(self):
                self.serial = serial.Serial(port, timeout=0, exclusive=True)

            async def start_notify(self, uuid, callback):
                if uuid == console.TX:
                    self.shell_callback = callback
                    asyncio.get_running_loop().add_reader(self.serial.fileno(), self.read)
                else:
                    assert uuid is self.smp
                    self.smp_callback = callback

            def notify_shell(self, data):
                for offset in range(0, len(data), 7):
                    self.shell_callback(None, data[offset : offset + 7])

            def notify_smp(self, data):
                for offset in range(0, len(data), 7):
                    self.smp_callback(None, data[offset : offset + 7])

            def read(self):
                self.decoder.feed(self.serial.read(127))

            async def write_gatt_char(self, characteristic, data, response):
                assert characteristic in (self.rx, self.smp)
                assert response == (characteristic is self.rx)
                assert len(data) <= characteristic.max_write_without_response_size
                self.max_write = max(self.max_write, len(data))
                if self.pause_writes:
                    await asyncio.Event().wait()
                if characteristic is self.smp:
                    self.smp_request.extend(data)
                    if len(self.smp_request) < header.Header.SIZE:
                        return
                    size = (
                        header.Header.loads(self.smp_request[: header.Header.SIZE]).length
                        + header.Header.SIZE
                    )
                    if len(self.smp_request) < size:
                        return
                    assert len(self.smp_request) == size
                    wire = b"".join(packet.encode(bytes(self.smp_request)))
                    self.smp_request.clear()
                else:
                    wire = data
                for offset in range(0, len(wire), 20):
                    chunk = wire[offset : offset + 20]
                    while chunk:
                        chunk = chunk[self.serial.write(chunk) :]
                        await asyncio.sleep(0)

            async def disconnect(self):
                if self.serial:
                    asyncio.get_running_loop().remove_reader(self.serial.fileno())
                    self.serial.close()
                self.disconnected(self)

        async def discover(**kwargs):
            return [SimpleNamespace(name="fixture", address="synthetic")]

        if ble:
            monkeypatch.setattr(console, "BleakClient", FakeRadio)
            monkeypatch.setattr(console.BleakScanner, "discover", discover)

        async def exercise():
            output = bytearray()
            target = {"ble": "fixture"} if ble else {"port": port}
            async with console.connection(output.extend, management=True, **target) as (
                stream,
                client,
            ):
                assert "power" in (await telemetry.info(client)).sources
                if ble:
                    assert 0 < instances[0].max_write <= 20
                    instances[0].rx.max_write_without_response_size = 61
                    instances[0].smp.max_write_without_response_size = 61
                # SMP traffic between pieces of a human command cannot enter
                # Shell editing state; keyboard traffic cannot split SMP frames.
                await stream.write(b"config set power/usb_idle_s 19")
                echo, _ = await asyncio.gather(
                    checked(client, EchoWrite(d="x" * 330)), stream.write(b"\x7f2\r")
                )
                assert echo.r == "x" * 330
                if ble:
                    assert instances[0].max_write == 61
                assert (await telemetry.snapshot(client, "power")).values["usb_idle_s"] == 12
                await stream.write(b"config get power/usb_idle_s\r")
                async with asyncio.timeout(2):
                    while b"12 s (selected and effective)" not in output:
                        await asyncio.sleep(0.01)
                assert packet.START_DELIMITER not in output
                assert packet.CONTINUE_DELIMITER not in output
                # The actual hub history still owns the previous command.
                output.clear()
                await stream.write(b"\x1b[A\r")
                async with asyncio.timeout(2):
                    while b"12 s (selected and effective)" not in output:
                        await asyncio.sleep(0.01)
                if ble:
                    instances[0].pause_writes = True
                    with pytest.raises(TimeoutError):
                        await client.request(EchoWrite(d="timeout fixture"), timeout_s=0.03)
                    assert stream.closed.is_set()
                    with pytest.raises(SMPTransportDisconnected, match="reconnect"):
                        await stream.write(b"late command\r")
            assert stream.closed.is_set()
            if ble:
                assert len(instances) == 1 and not instances[0].serial.is_open
            else:
                assert not stream.serial.is_open

        asyncio.run(exercise())


def test_monitor_cli_recording(tmp_path):
    log = tmp_path / "capture.jsonl"
    cli = [Path(sys.executable).with_name("brickphyrdev"), "monitor"]
    with firmware(tmp_path / "flash.bin") as port:
        result = subprocess.run(
            [*cli, "--port", port, "--count", "2", "--rate", "20", "--log", str(log)],
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        assert "power: unavailable" in result.stdout and "matrix: unavailable" in result.stdout
        records = [json.loads(line) for line in log.read_text().splitlines()]
        assert records[0]["schema"] == "brickphyr.telemetry/1"
        assert "native_sim" in records[0]["board"]
        assert records[-1]["reason"] == "completed" and records[-1]["records"] == 4
        snapshots = records[1:-1]
        assert [x["source"] for x in snapshots] == ["power", "matrix"] * 2
        assert snapshots[0]["values"]["sample_ms"] == snapshots[2]["values"]["sample_ms"] == 0
        assert snapshots[1]["values"]["applied"] == [0] * 25
        assert snapshots[0]["received_monotonic_ns"] < snapshots[2]["received_monotonic_ns"]
        before = log.read_bytes()
        rejected = subprocess.run(
            [*cli, "--port", port, "--count", "1", "--log", str(log)],
            capture_output=True,
            text=True,
            timeout=3,
        )
        assert rejected.returncode != 0 and log.read_bytes() == before
        invalid = subprocess.run(
            [*cli, "--port", port, "--count", "1", "--source", "missing"],
            capture_output=True,
            text=True,
            timeout=3,
        )
        assert invalid.returncode != 0 and "Unavailable sources" in invalid.stderr


def test_interactive_terminal_preserves_line_and_tty(tmp_path):
    with firmware(tmp_path / "flash.bin") as port:
        master, slave = os.openpty()
        original = termios.tcgetattr(slave)
        log = tmp_path / "terminal.jsonl"
        process = subprocess.Popen(
            [
                Path(sys.executable).with_name("brickphyrdev"),
                "terminal",
                "--port",
                port,
                "--monitor",
                "--source",
                "power",
                "--rate",
                "10",
                "--log",
                str(log),
            ],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            start_new_session=True,
            preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY),
        )
        output = bytearray()
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(master, selectors.EVENT_READ)

                def until(marker):
                    deadline = time.monotonic() + 3
                    while marker not in output:
                        assert process.poll() is None, output.decode(errors="replace")
                        remaining = deadline - time.monotonic()
                        assert remaining > 0 and selector.select(remaining), output.decode(
                            errors="replace"
                        )
                        output.extend(os.read(master, 8192))

                until(b"Ctrl+T")
                # Wait for successful observations, not for a startup delay.
                deadline = time.monotonic() + 3
                while not log.exists() or '"type":"snapshot"' not in log.read_text():
                    assert process.poll() is None and time.monotonic() < deadline
                    time.sleep(0.01)
                os.write(master, b"config get power/usb_idle_")
                until(b"power/usb_idle_")
                os.write(master, b"\x14")
                until(b"power: unavailable")
                assert b"\x1b[?1049h" in output
                before = len(log.read_text().splitlines())
                deadline = time.monotonic() + 2
                while len(log.read_text().splitlines()) <= before:
                    assert time.monotonic() < deadline
                    time.sleep(0.01)
                os.write(master, b"\x14s\r")
                until(b"0 s (selected and effective)")
                assert b"\x1b[?1049l" in output
                os.write(master, b"\x1d")
                assert process.wait(timeout=3) == 0, output.decode(errors="replace")
            assert termios.tcgetattr(slave) == original
            assert json.loads(log.read_text().splitlines()[-1])["reason"] == "cancelled"
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)
            os.close(master)
            os.close(slave)


def test_poll_overrun_and_recording_failure(tmp_path):
    with firmware(tmp_path / "flash.bin") as port:

        async def exercise():
            async with console.connection(lambda _: None, port=port, management=True) as (
                _,
                client,
            ):
                identity, sources = await monitor.prepare(client, ["power"])
                times = []

                def slow_output(records, missed):
                    times.append(time.monotonic())
                    time.sleep(0.075)  # Explicitly synthetic slow terminal/disk.

                with monitor.recording(tmp_path / "slow.jsonl") as log:
                    await monitor.watch(client, identity, sources, 20, log, slow_output, count=2)
                records = [
                    json.loads(x) for x in (tmp_path / "slow.jsonl").read_text().splitlines()
                ]
                assert records[-1]["missed_polls"] >= 2
                assert times[1] - times[0] >= 0.095, "A slow cycle caused a catch-up burst"

                class BrokenDisk:
                    def write(self, data):
                        raise OSError("synthetic disk full")

                with pytest.raises(OSError, match="disk full"):
                    await monitor.watch(
                        client, identity, sources, 2, BrokenDisk(), lambda *_: None, count=1
                    )
                assert (await telemetry.snapshot(client, "power")).values["connected"] == 1

        asyncio.run(exercise())


@pytest.mark.parametrize("bad_port", [False, True])
def test_terminal_restores_tty_on_failure(tmp_path, bad_port):
    with firmware(tmp_path / "flash.bin") as port:
        master, slave = os.openpty()
        original = termios.tcgetattr(slave)
        target = str(tmp_path / "absent-port") if bad_port else port
        try:
            result = subprocess.run(
                [
                    Path(sys.executable).with_name("brickphyrdev"),
                    "terminal",
                    "--port",
                    target,
                    "--monitor",
                    "--source",
                    "absent-source",
                ],
                stdin=slave,
                stdout=slave,
                stderr=slave,
                timeout=3,
            )
            assert result.returncode != 0
            assert termios.tcgetattr(slave) == original
            output = os.read(master, 8192)
            assert b"absent-port" in output if bad_port else b"Unavailable sources" in output
        finally:
            os.close(master)
            os.close(slave)
