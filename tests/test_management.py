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

import serial
from smpclient import SMPClient, error, success
from smpclient.requests.os_management import EchoWrite
from smpclient.requests.settings_management import ReadSetting, SaveSettings, WriteSetting
from smpclient.transport.serial import SMPSerialTransport


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
