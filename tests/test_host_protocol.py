# SPDX-License-Identifier: Apache-2.0
"""Use the upstream Python USB client against the production C protocol handler."""

import asyncio
import errno
import io
import os
import pty
import subprocess
import sys
import tty
from pathlib import Path

import pytest
import usb.core
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


def test_status_repeated_after_missed_subscription_event(protocol_bridge):
    subprocess.run([protocol_bridge, "status"], check=True, timeout=2)


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
        return str(hub.fw_version)

    try:
        firmware_version = asyncio.run(check())
        result = subprocess.run(
            [sys.executable, "-m", "pybricksdev", "usb-info", os.ttyname(slave)],
            capture_output=True,
            text=True,
            timeout=3,
        )
        assert result.returncode == 0, result.stderr
        assert "Name: Brickphyr" in result.stdout
        assert f"Firmware: {firmware_version}" in result.stdout

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


def test_shell_handover_with_immediate_prompt(monkeypatch):
    from brickphyrdev import usb as brickphyr_usb

    state = ["shell", False, True]
    commands = []

    class Port(io.BytesIO):
        """Synthetic line-control peer: open discards any prompt already sent."""

        def __init__(self, port=None, *args, **kwargs):
            super().__init__()
            self.is_open = False
            self.dtr = True
            if port is not None:
                self.open()

        @property
        def dtr(self):
            return self._dtr

        @dtr.setter
        def dtr(self, value):
            self._dtr = value
            if self.is_open and value:
                self.write_reply(b"brickphyr> ")

        def write_reply(self, data):
            self.seek(0)
            self.truncate()
            super().write(data)
            self.seek(0)

        def open(self):
            self.is_open = True
            self.dtr = self.dtr
            self.write_reply(b"")

        def read_until(self, delimiter):
            return self.read()

        def write(self, data):
            commands.append(data)
            self.write_reply(b"Pybricks selected for the next session.\r\nbrickphyr> ")

        def close(self):
            if commands:
                state[0] = "pybricks"
            super().close()

    monkeypatch.setattr(brickphyr_usb.serial, "Serial", Port)
    monkeypatch.setattr(brickphyr_usb, "serial_port", lambda hub: "synthetic")
    monkeypatch.setattr(brickphyr_usb, "status", lambda hub: tuple(state))
    brickphyr_usb.handover(object(), "pybricks")
    assert commands == [b"host_mode pybricks\r"]
    assert state[0] == "pybricks"


class _UsbHub:
    iManufacturer = 1
    iSerialNumber = 2
    langids = usb.core.Device.langids
    serial_number = usb.core.Device.serial_number

    def __init__(self, manufacturer="Brickphyr", serial_number="hub-1", *, phase=2,
                 descriptor_error=None, languages=(0x0409,)):
        self.manufacturer_name = manufacturer
        self.serial_text = serial_number
        self.phase = phase
        self.descriptor_error = descriptor_error
        self.languages = languages
        self.set_calls = 0
        self._langids = None
        self._serial_number = None

    def ctrl_transfer(self, bmRequestType, bRequest, wValue=0, wIndex=0,
                      data_or_wLength=None, timeout=None):
        import array

        import usb.core

        if (bmRequestType, bRequest) == (0x80, 0x06):
            if self.descriptor_error:
                raise self.descriptor_error
            index = wValue & 0xFF
            if index == 0:
                body = b"".join(lang.to_bytes(2, "little") for lang in self.languages)
            else:
                assert wIndex == self.languages[0]
                value = self.manufacturer_name if index == 1 else self.serial_text
                body = value.encode("utf-16-le")
            return array.array("B", bytes((len(body) + 2, 3)) + body)
        assert (bRequest, wValue, wIndex) == (0x43, 0, 0)
        if bmRequestType == 0xC0:
            return bytes((1, self.phase))
        assert (bmRequestType, data_or_wLength) == (0x40, b"")
        self.set_calls += 1
        raise usb.core.USBError("reset during SET acknowledgment", errno=errno.ENODEV)


def test_usb_wake_survives_reset_ack_race(monkeypatch, capsys):
    import usb.core
    import usb.util

    from brickphyrdev import usb as brickphyr_usb
    from brickphyrdev.cli import main

    released = []
    off = _UsbHub(phase=2)
    running = _UsbHub(phase=0)
    discoveries = iter(([off], [], [running]))
    clock = [0.0]
    monkeypatch.setattr(usb.core, "find", lambda **kwargs: next(discoveries))
    monkeypatch.setattr(usb.util, "dispose_resources", released.append)
    monkeypatch.setattr(brickphyr_usb.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(brickphyr_usb.time, "sleep", lambda seconds: clock.__setitem__(0, clock[0] + seconds))
    monkeypatch.setattr(sys, "argv", ["brickphyrdev", "usb", "wake"])

    main()

    assert capsys.readouterr().out == "USB running\n"
    assert off.set_calls == 1
    assert running.set_calls == 0
    assert released == [running, off]


def test_usb_discovery_closes_unselected_and_error_handles(monkeypatch):
    import usb.core
    import usb.util

    from brickphyrdev import usb as brickphyr_usb

    foreign, wrong, selected = _UsbHub("Other"), _UsbHub(serial_number="hub-2"), _UsbHub()
    released = []
    monkeypatch.setattr(usb.util, "dispose_resources", released.append)
    monkeypatch.setattr(usb.core, "find", lambda **kwargs: [foreign, wrong, selected])
    assert brickphyr_usb.device("hub-1") is selected
    assert released == [foreign, wrong]

    duplicate = _UsbHub()
    monkeypatch.setattr(usb.core, "find", lambda **kwargs: [selected, duplicate])
    with pytest.raises(RuntimeError, match="found 2"):
        brickphyr_usb.device("hub-1", missing_ok=True)
    assert released[-2:] == [selected, duplicate]

    failure = usb.core.USBError("permission denied", errno=errno.EACCES)
    denied = _UsbHub(descriptor_error=failure)
    monkeypatch.setattr(usb.core, "find", lambda **kwargs: [denied])
    with pytest.raises(usb.core.USBError) as caught:
        brickphyr_usb.device("hub-1", missing_ok=True)
    assert caught.value is failure
    assert released[-1] is denied

    no_languages = _UsbHub(languages=())
    monkeypatch.setattr(usb.core, "find", lambda **kwargs: [no_languages])
    assert brickphyr_usb.device("hub-1", missing_ok=True) is None
    assert released[-1] is no_languages


def test_usb_wake_retries_enumeration_reset_and_preserves_permission_error(monkeypatch):
    import usb.core
    import usb.util

    from brickphyrdev import usb as brickphyr_usb

    reset = usb.core.USBError("reset during descriptor read", errno=errno.ENODEV)
    denied = usb.core.USBError("permission denied during descriptor read", errno=errno.EACCES)
    off = _UsbHub()
    resetting = _UsbHub(descriptor_error=reset)
    running = _UsbHub(phase=0)
    clock = [0.0]
    released = []
    discoveries = iter(([resetting], [running]))
    monkeypatch.setattr(usb.core, "find", lambda **kwargs: next(discoveries))
    monkeypatch.setattr(usb.util, "dispose_resources", released.append)
    monkeypatch.setattr(brickphyr_usb.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(brickphyr_usb.time, "sleep", lambda seconds: clock.__setitem__(0, clock[0] + seconds))

    brickphyr_usb.wake(off)
    assert off.set_calls == 1
    assert released == [resetting, running]

    forbidden = _UsbHub(descriptor_error=denied)
    monkeypatch.setattr(usb.core, "find", lambda **kwargs: [forbidden])
    with pytest.raises(usb.core.USBError) as caught:
        brickphyr_usb.wake(_UsbHub())
    assert caught.value is denied
    assert released[-1] is forbidden


def test_usb_wake_initial_identity_error_sends_no_set(monkeypatch):
    import usb.core

    from brickphyrdev import usb as brickphyr_usb

    failure = usb.core.USBError("reset during initial serial read", errno=errno.ENODEV)
    hub = _UsbHub(descriptor_error=failure)
    monkeypatch.setattr(usb.core, "find", lambda **kwargs: pytest.fail("unidentified hub rediscovered"))

    with pytest.raises(usb.core.USBError) as caught:
        brickphyr_usb.wake(hub)
    assert caught.value is failure
    assert hub.set_calls == 0


def test_usb_wake_running_is_no_op():
    from brickphyrdev import usb as brickphyr_usb

    hub = _UsbHub(phase=0)
    brickphyr_usb.wake(hub)
    assert hub.set_calls == 0


def test_usb_wake_times_out_without_repeating_set(monkeypatch):
    import usb.core
    import usb.util

    from brickphyrdev import usb as brickphyr_usb

    sets = []
    released = []
    clock = [0.0]
    failure = usb.core.USBError("reset during SET acknowledgment", errno=errno.ENODEV)

    class Hub(_UsbHub):
        def ctrl_transfer(self, bmRequestType, bRequest, wValue=0, wIndex=0,
                          data_or_wLength=None, timeout=None):
            if bmRequestType == 0x40:
                sets.append(self)
                raise failure
            return super().ctrl_transfer(bmRequestType, bRequest, wValue, wIndex,
                                         data_or_wLength, timeout)

    first = Hub()
    monkeypatch.setattr(usb.core, "find", lambda **kwargs: [Hub()])
    monkeypatch.setattr(usb.util, "dispose_resources", released.append)
    monkeypatch.setattr(brickphyr_usb.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(brickphyr_usb.time, "sleep", lambda seconds: clock.__setitem__(0, clock[0] + seconds))

    with pytest.raises(TimeoutError, match="within 10 seconds") as caught:
        brickphyr_usb.wake(first)
    assert caught.value.__cause__ is failure
    assert sets == [first]
    assert released and all(hub is not first for hub in released)


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
