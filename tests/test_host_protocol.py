# SPDX-License-Identifier: Apache-2.0
"""Use the upstream Python USB client against the production C protocol handler."""

import asyncio
import io
import os
import pty
import subprocess
import sys
import tty
from pathlib import Path

import pytest
from west.manifest import Manifest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def protocol_bridge(tmp_path_factory):
    pybricks = Path(Manifest.from_topdir().get_projects(["pybricks"])[0].abspath)
    executable = tmp_path_factory.mktemp("protocol") / "bridge"
    subprocess.run(
        [
            "cc",
            "-std=c17",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include'}",
            f"-I{ROOT / 'build/prime/zephyr/include/generated'}",
            f"-I{pybricks / 'lib/pbio/include'}",
            str(ROOT / "tests/host/protocol_bridge.c"),
            str(ROOT / "src/host_protocol.c"),
            str(pybricks / "lib/pbio/src/cobs.c"),
            str(pybricks / "lib/pbio/src/protocol/pybricks.c"),
            "-o",
            str(executable),
        ],
        check=True,
    )
    return executable


@pytest.fixture(scope="module")
def protocol(protocol_bridge):
    pybricks = Path(Manifest.from_topdir().get_projects(["pybricks"])[0].abspath)
    with pytest.MonkeyPatch.context() as patch:
        patch.syspath_prepend(str(pybricks / "tools/flash"))
        from serdev import pybricks as client

    def exchange(request):
        output = subprocess.run(
            [protocol_bridge], input=request, capture_output=True, check=True, timeout=2
        ).stdout
        stream = io.BytesIO(output)
        replies = []
        while stream.tell() < len(output):
            replies.append(client.read_frame(stream))
        return replies

    return client, exchange


def test_pybricksdev_serial_client(protocol_bridge, monkeypatch, capsys):
    import usb.util
    from pybricksdev.ble.pybricks import PYBRICKS_COMMAND_EVENT_UUID, SW_REV_UUID
    from pybricksdev.connections.serial import PybricksHubSerial

    from brickphyrdev import usb as brickphyr_usb
    from brickphyrdev.cli import main

    master, slave = pty.openpty()
    tty.setraw(slave)
    process = subprocess.Popen([protocol_bridge], stdin=master, stdout=master)

    async def check():
        hub = PybricksHubSerial(os.ttyname(slave), timeout=0.5)
        try:
            for _ in range(2):
                await hub.connect()
                assert hub.name == "Brickphyr"
                assert hub.fw_version.local == "bringup"
                assert hub.hub_kind == 0x81
                status = asyncio.Event()
                with hub.status_observable.subscribe(
                    lambda flags: status.set() if int(flags) == 1 << 12 else None
                ):
                    await asyncio.wait_for(status.wait(), 0.5)
                with pytest.raises(RuntimeError, match="Write failed: 128"):
                    await hub.stop_user_program()
                assert await hub.read_gatt_char(SW_REV_UUID) == b"1.7.0"
                # Brickphyr's explicit extension, sent through the public client API.
                await hub.write_gatt_char(PYBRICKS_COMMAND_EVENT_UUID, b"\x80", True)
                await hub.disconnect()
        finally:
            await hub.disconnect()

    try:
        asyncio.run(check())
        result = subprocess.run(
            [sys.executable, "-m", "pybricksdev", "usb-info", os.ttyname(slave)],
            capture_output=True,
            text=True,
            timeout=3,
        )
        assert result.returncode == 0, result.stderr
        assert "Name: Brickphyr" in result.stdout
        assert "Firmware: 0.1.0+bringup" in result.stdout

        # Only USB enumeration/control is simulated; identity and handover use the
        # installed client over the PTY against the production firmware handler.
        hub = object()
        released = []
        monkeypatch.setattr(brickphyr_usb, "device", lambda serial: hub)
        monkeypatch.setattr(brickphyr_usb, "serial_port", lambda hub: os.ttyname(slave))
        monkeypatch.setattr(brickphyr_usb, "status", lambda hub: ("pybricks", False, True))
        monkeypatch.setattr(usb.util, "dispose_resources", released.append)
        monkeypatch.setattr(sys, "argv", ["brickphyrdev", "usb", "identify"])
        main()
        output = capsys.readouterr().out
        assert "name: Brickphyr" in output
        assert "protocol: 1.7.0" in output
        assert released == [hub]

        statuses = iter(
            (("pybricks", False, True), ("pybricks", True, False), ("shell", False, True))
        )
        monkeypatch.setattr(
            brickphyr_usb, "status", lambda hub: next(statuses, ("shell", False, True))
        )
        monkeypatch.setattr(sys, "argv", ["brickphyrdev", "usb", "handover", "shell"])
        main()
        assert "shell: idle, selectable" in capsys.readouterr().out
        assert released == [hub, hub]
    finally:
        process.terminate()
        process.wait(timeout=2)
        os.close(master)
        os.close(slave)


def test_identification_and_unavailable_capabilities(protocol):
    client, exchange = protocol
    requests = io.BytesIO()
    for service, characteristic in (
        (1, 0x2A00),
        (1, 0x2A26),
        (1, 0x2A28),
        (1, 0x2A50),
        (2, 3),
        (1, 0xFFFF),
    ):
        client.send_read(requests, service, characteristic)
    replies = exchange(requests.getvalue())
    assert replies[0] == b"\x03\x01\x00\x2aBrickphyr"
    assert replies[1].startswith(b"\x03\x01\x26\x2a0.")
    assert replies[2] == b"\x03\x01\x28\x2a1.7.0"
    assert replies[3] == bytes.fromhex("03 01 50 2a 01 97 03 81 00 01 00")
    assert replies[4][:4] == bytes.fromhex("03 02 03 00")
    assert 0 < int.from_bytes(replies[4][4:6], "little") <= 64
    assert replies[4][6:] == bytes(9)  # No engines, storage or program slots.
    assert replies[5] == bytes.fromhex("03 01 ff ff")


def test_command_errors_and_subscription(protocol):
    client, exchange = protocol
    requests = io.BytesIO()
    for tag, command in ((7, 0), (8, 1), (9, 3), (10, 5), (11, 255)):
        client.send_frame(requests, bytes((2, tag, command)))
    client.send_frame(requests, b"\x01\x01")
    client.send_frame(requests, b"\x01\x00")
    replies = exchange(requests.getvalue())
    assert replies[:5] == [bytes((1, tag, 0x80, 0, 0, 0)) for tag in range(7, 12)]
    assert replies[5:] == [bytes.fromhex("02 00 00 10 00 00 00 00")]


def test_framing_recovery_and_invalid_requests(protocol):
    client, exchange = protocol
    read = io.BytesIO()
    client.send_read(read, 1, 0x2A00)
    valid = read.getvalue()
    assert exchange(b"x" * 2000 + b"\x02" + valid) == exchange(valid)
    requests = io.BytesIO()
    for message in (b"\x03\x01", b"\x03\x01\x00\x2a\xff", b"\x01\x02", b"\xff\x01"):
        client.send_frame(requests, message)
    assert exchange(requests.getvalue() + valid) == exchange(valid)
    assert exchange(b"\x02\x02\x01" + valid) == exchange(valid)
    # A final COBS block declares more bytes than are present. Never interpret it.
    assert exchange(bytes.fromhex("5b 00 00 06 29 02") + valid) == exchange(valid)


def test_exit_compatibility_command(protocol):
    client, exchange = protocol
    requests = io.BytesIO()
    client.send_frame(requests, bytes((2, 31, 0x80, 0)))
    client.send_frame(requests, bytes((2, 32, 0x80)))
    assert exchange(requests.getvalue()) == [
        bytes((1, 31, 0x13, 0, 0, 0)),
        bytes((1, 32, 0, 0, 0, 0)),
    ]
