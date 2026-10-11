# SPDX-License-Identifier: Apache-2.0
"""Run the unmodified SMP client against the actual Zephyr management stack."""

import asyncio
import os
import re
import selectors
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path

import pytest
import serial
from smpclient import SMPClient, error, success
from smpclient.requests.os_management import EchoWrite
from smpclient.requests.settings_management import ReadSetting, SaveSettings, WriteSetting
from smpclient.transport.serial import SMPSerialTransport

from brickphyrdev.smp import checked
from brickphyrdev.telemetry import Snapshot, info, snapshot


@contextmanager
def firmware(flash):
    executable = Path(__file__).resolve().parents[1] / "build/management-tests/zephyr/zephyr.exe"
    process = subprocess.Popen(
        [executable, f"--flash={flash}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        bufsize=0,
    )
    try:
        output = bytearray()
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while not (match := re.search(rb"pseudotty: (\S+)\n", output)):
                assert selector.select(3), output.decode()
                chunk = os.read(process.stdout.fileno(), 4096)
                assert chunk, output.decode()
                output.extend(chunk)
        port = match[1].decode()
        # PTY creation precedes firmware initialization. Wait for the actual
        # Shell session before sending the client's first parameter request.
        with serial.Serial(port, timeout=3, exclusive=True) as console:
            prompt = console.read_until(b"brickphyr> ")
            assert prompt.endswith(b"brickphyr> "), prompt
        yield port
    finally:
        process.terminate()
        process.wait(timeout=3)
        process.stdout.close()


def test_smp_settings_and_restart(tmp_path):
    flash = tmp_path / "flash.bin"
    name = "power/usb_idle_s"

    async def first_boot(port):
        client = SMPClient(SMPSerialTransport(exclusive=True), port, timeout_s=1)
        try:
            await client.connect()
            reply = await client.request(EchoWrite(d="Brickphyr SMP"))
            assert success(reply) and reply.r == "Brickphyr SMP", reply
            # One packet spanning several serial lines and the UART RX ring.
            message = "x" * 330
            reply = await client.request(EchoWrite(d=message))
            assert success(reply) and reply.r == message, reply
            assert success(
                await client.request(WriteSetting(name=name, val=(42).to_bytes(4, "little")))
            )
            assert success(await client.request(SaveSettings()))
            assert success(
                await client.request(WriteSetting(name=name, val=(43).to_bytes(4, "little")))
            )
            assert error(await client.request(WriteSetting(name=name, val=b"\x01")))
            reply = await client.request(ReadSetting(name=name))
            assert success(reply) and int.from_bytes(reply.val, "little") == 43, reply
        finally:
            await client.disconnect()

    with firmware(flash) as port:
        asyncio.run(first_boot(port))
        result = subprocess.run(
            [Path(sys.executable).with_name("smpmgr"), "--port", port, "os", "echo", "Brickphyr CLI"],
            capture_output=True,
            text=True,
            timeout=3,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        assert "Brickphyr CLI" in result.stdout, result.stdout
        with serial.Serial(port, timeout=1, write_timeout=1, exclusive=True) as console:
            console.write(f"config get {name}\r".encode())
            output = console.read_until(b"brickphyr> ")
            assert b"43 s (selected and effective)" in output, output
            assert b"Saved: 42 s (RAM differs)" in output, output

    async def after_restart(port):
        client = SMPClient(SMPSerialTransport(exclusive=True), port, timeout_s=1)
        try:
            await client.connect()
            reply = await client.request(ReadSetting(name=name))
            assert success(reply) and int.from_bytes(reply.val, "little") == 42, reply
        finally:
            await client.disconnect()

    with firmware(flash) as port:
        asyncio.run(after_restart(port))


def test_read_only_observations(tmp_path):
    async def exercise(port):
        client = SMPClient(SMPSerialTransport(exclusive=True), port, timeout_s=1)
        try:
            await client.connect()
            identity = await info(client)
            assert identity.sources == ["power", "matrix"] and "native_sim" in identity.board
            # This board has no VBUS acquisition. A cold first read must report
            # unavailable data, not initialize a fictitious power source.
            first = await snapshot(client, "power")
            assert first.error < 0 and first.values["sample_ms"] == 0
            assert first.values["connected"] == 1
            name = "power/usb_idle_s"
            await checked(client, WriteSetting(name=name, val=(7).to_bytes(4, "little")))
            second = await snapshot(client, "power")
            assert second.values["usb_idle_s"] == 7
            assert second.read_ms >= first.read_ms
            assert second.values["sample_ms"] == first.values["sample_ms"]
            for source in ("buttons.center", "buttons.side", "application"):
                missing = await snapshot(client, source)
                assert missing.error < 0 and missing.values == {}, missing
            matrix = await snapshot(client, "matrix")
            assert matrix.error < 0 and matrix.values["valid"] == 0
            assert matrix.values["owner"] == 0
            with pytest.raises(RuntimeError, match="Snapshot rejected"):
                await snapshot(client, "motor.unknown")
            # The telemetry group exposes reads only. A write must be rejected.
            from smp.header import OP

            class Write(Snapshot):
                _OP = OP.WRITE

            rejected = await client.request(Write(source="power"))
            assert error(rejected) and int(rejected.rc) == 8, rejected
            assert (await snapshot(client, "power")).values == second.values
        finally:
            await client.disconnect()

    with firmware(tmp_path / "flash.bin") as port:
        asyncio.run(exercise(port))
        with serial.Serial(port, timeout=1, write_timeout=1, exclusive=True) as console:
            console.write(b"observe all\r")
            output = console.read_until(b"brickphyr> ")
            assert b"power read_ms=" in output and b"usb_idle_s=7" in output, output
            assert b"application read_ms=" not in output, output
            console.write(b"matrix pixel 0 4 100\r")
            assert console.read_until(b"brickphyr> ").endswith(b"brickphyr> ")

        async def matrix_read(port):
            client = SMPClient(SMPSerialTransport(exclusive=True), port, timeout_s=1)
            try:
                await client.connect()
                first = await snapshot(client, "matrix")
                assert first.error == 0 and first.values["valid"] == 1
                assert first.values["owner"] == 2 and first.values["pending"] == 0
                frame = bytes([0] * 4 + [100] + [0] * 20)
                assert first.values["requested"] == first.values["applied"] == frame
                assert 0 < first.values["applied_ms"] <= first.read_ms
                assert (await snapshot(client, "matrix")).values == first.values
            finally:
                await client.disconnect()

        asyncio.run(matrix_read(port))
