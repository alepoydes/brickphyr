# SPDX-License-Identifier: Apache-2.0
"""Firmware update admission and handoff; USB and erase/write are synthetic."""

import asyncio
import contextlib
import errno
import json
import struct
from pathlib import Path
from types import SimpleNamespace
from zipfile import ZipFile

import pytest

from brickphyrdev import flash
from brickphyrdev.command_file import Execute


@pytest.fixture
def package(tmp_path):
    # Synthetic F4 image with the independently specified mboot records/vectors.
    image = bytearray(1024)
    struct.pack_into("<II", image, 0, 0x20050000, 0x08010301)
    for offset in (0x1f0, 0x264):
        struct.pack_into("<B15sB15s", image, offset, 1, b"STM32F413", 0, b"SPIKE Prime")
    metadata = {"device-id": 129, "metadata-version": "3.0.0", "firmware-version": "test",
                "variants": [{"platform": "prime_hub_f4", "checksum-type": "none",
                              "hub-name-offset": 0, "hub-name-size": 0,
                              "firmware": "firmware-base.bin"}]}

    def write(*, corrupt=False, platform="prime_hub_f4"):
        metadata["variants"][0]["platform"] = platform
        if corrupt:
            struct.pack_into("<I", image, 4, 0x08008001)  # Preserved loader, not this image.
        path = tmp_path / "firmware.zip"
        with ZipFile(path, "w") as archive:
            archive.writestr("firmware.metadata.json", json.dumps(metadata))
            archive.writestr("firmware-base.bin", image)
            archive.writestr("ReadMe_OSS.txt", "Experimental Brickphyr prime_hub_f4 firmware.")
        return path
    return write


def arguments(tmp_path, package):
    installer = tmp_path / "pybricks/tools/flash"
    installer.mkdir(parents=True)
    (installer / "__main__.py").touch()
    return SimpleNamespace(firmware=package, pybricks=tmp_path / "pybricks")


@pytest.mark.parametrize("mutation", [{"corrupt": True}, {"platform": "technic_hub"}])
def test_invalid_package_never_touches_hub(tmp_path, monkeypatch, package, mutation):
    args = arguments(tmp_path, package(**mutation))
    monkeypatch.setattr(flash.subprocess, "run", lambda *a, **kw: None)
    monkeypatch.setattr(flash, "attached", lambda: pytest.fail("Invalid image reached USB"))
    with pytest.raises(ValueError, match="Reset vector|prime_hub_f4"):
        flash.run(args)


@pytest.mark.parametrize("initial", ["running", "dfu"])
def test_update_uses_validated_snapshot_and_checks_return(tmp_path, monkeypatch, package, initial):
    args = arguments(tmp_path, package())
    original = args.firmware.read_bytes()
    events = []
    monkeypatch.setattr(flash, "attached", lambda: ((1, (2,)), initial))

    async def enter():
        events.append("stop/reboot/close")
        args.firmware.write_bytes(b"concurrent rebuild")

    def wait(identity, mode):
        assert identity == (1, (2,))
        events.append(mode)

    def installer(command, **kw):
        if command[-1] != "--help":
            assert Path(command[-1]).read_bytes() == original
            events.append("write")

    async def verify(version):
        assert version == "test"
        events.append("verify")
        return "Brickphyr test"

    monkeypatch.setattr(flash, "enter_dfu", enter)
    monkeypatch.setattr(flash, "wait_for", wait)
    monkeypatch.setattr(flash.subprocess, "run", installer)
    monkeypatch.setattr(flash, "verify", verify)
    flash.run(args)
    assert events == (["stop/reboot/close"] if initial == "running" else []) + [
        "dfu", "write", "running", "verify"]


@pytest.mark.parametrize("state,cleanup", [(1, 0), (3, -5), (3, 0)])
def test_stop_must_quiesce_before_single_reboot(monkeypatch, state, cleanup):
    events = []

    async def request(client, request):
        events.append(type(request).__name__)
        if isinstance(request, Execute):
            return SimpleNamespace(ret=0, truncated=False, o=
                "Brickphyr test (fixture)\nTarget spike_prime_f4/stm32f413xx\n")
        return SimpleNamespace(state=state, cleanup=cleanup)

    @contextlib.asynccontextmanager
    async def connection(*a, **kw):
        closed = asyncio.Event()

        async def write(data):
            assert data == b"reboot dfu\r"
            events.append("reboot")
            closed.set()

        try:
            yield SimpleNamespace(write=write, closed=closed), None
        finally:
            events.append("close")

    monkeypatch.setattr(flash, "checked", request)
    monkeypatch.setattr(flash.console, "connection", connection)
    if state == 1 or cleanup:
        with pytest.raises(RuntimeError, match="not quiescent"):
            asyncio.run(flash.enter_dfu())
        assert "reboot" not in events
    else:
        asyncio.run(flash.enter_dfu())
        assert events[-2:] == ["reboot", "close"]
    assert events[:4] == ["Execute", "Status", "Stop", "Status"]
    assert events[-1] == "close"


def test_ambiguous_hubs_rejected_and_handles_released(monkeypatch):
    devices = [object(), object()]
    released = []
    monkeypatch.setattr(flash.usb.core, "find", lambda **kw: devices)
    monkeypatch.setattr(flash.usb.util, "dispose_resources", released.append)
    with pytest.raises(RuntimeError, match="only one"):
        flash.attached()
    assert released == devices


def test_wrong_returning_usb_port_rejected(monkeypatch):
    monkeypatch.setattr(flash, "attached", lambda: ((1, (3,)), "dfu"))
    with pytest.raises(RuntimeError, match="different hub"):
        flash.wait_for((1, (2,)), "dfu")


def test_usb_reset_retried_but_permission_error_preserved(monkeypatch):
    def resetting():
        monkeypatch.setattr(flash, "attached", lambda: ((1, (2,)), "dfu"))
        raise flash.usb.core.USBError("Device vanished", errno=errno.ENODEV)

    monkeypatch.setattr(flash, "attached", resetting)
    monkeypatch.setattr(flash.time, "sleep", lambda _: None)
    flash.wait_for((1, (2,)), "dfu")

    def denied():
        raise flash.usb.core.USBError("Permission denied", errno=errno.EACCES)

    monkeypatch.setattr(flash, "attached", denied)
    with pytest.raises(flash.usb.core.USBError, match="Permission denied"):
        flash.wait_for((1, (2,)), "dfu")


def test_wrong_firmware_return_rejected(monkeypatch):
    async def checked(*a):
        return SimpleNamespace(ret=0, truncated=False, o=
            "Brickphyr old (fixture)\nTarget spike_prime_f4/stm32f413xx\n")
    monkeypatch.setattr(flash, "checked", checked)
    with pytest.raises(RuntimeError, match="Expected firmware new, found old"):
        asyncio.run(flash.info(None, "new"))
