# SPDX-License-Identifier: Apache-2.0
"""Install a checked Brickphyr F4 package using the existing Pybricks installer."""

import asyncio
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from tempfile import TemporaryDirectory
from zipfile import ZipFile

import usb.core
import usb.util
from smp.error import MGMT_ERR

from . import application, console
from . import usb as hub_usb
from .command_file import Execute
from .firmware import check_binary
from .smp import Rejected, checked


def check_package(path):
    with ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate firmware ZIP members")
        metadata = json.loads(archive.read("firmware.metadata.json"))
        variants = metadata.get("variants", [])
        if (metadata.get("device-id") != 129 or
                metadata.get("metadata-version") != "3.0.0" or len(variants) != 1):
            raise ValueError("Expected a Brickphyr SPIKE/Inventor F4 v3 firmware package")
        variant = variants[0]
        if (variant.get("platform") != "prime_hub_f4" or
                variant.get("checksum-type") != "none" or
                variant.get("hub-name-offset") != 0 or variant.get("hub-name-size") != 0):
            raise ValueError("Expected an unmodified prime_hub_f4 image")
        notice = archive.read("ReadMe_OSS.txt")
        if not notice.startswith(b"Experimental Brickphyr prime_hub_f4 firmware."):
            raise ValueError("Expected a Brickphyr firmware package")
        check_binary(archive.read(variant["firmware"]))
        version = metadata.get("firmware-version")
        if not isinstance(version, str) or not version:
            raise ValueError("Missing firmware version")
        return version


def attached():
    """One hub, including upstream auto-reboot candidates; never pick the first."""
    devices = list(usb.core.find(find_all=True, custom_match=lambda d:
        d.idVendor == 0x0694 or (d.idVendor, d.idProduct) == (0x2FE3, 0x0004)))
    try:
        if not devices:
            return None
        if len(devices) != 1:
            raise RuntimeError("Connect only one LEGO/Brickphyr hub before flashing")
        device = devices[0]
        if not device.port_numbers or device.bus is None:
            raise RuntimeError("USB topology is unavailable; cannot track the hub through DFU")
        identity = device.bus, tuple(device.port_numbers)
        if (device.idVendor, device.idProduct) == (0x2FE3, 0x0004):
            # PyUSB's manufacturer/langids properties can hide a reset error
            # behind ValueError. Keep the original USB error for bounded retry.
            languages = usb.util.get_langids(device)
            if not languages or usb.util.get_string(
                    device, device.iManufacturer, languages[0]) != "Brickphyr":
                raise RuntimeError("The connected USB device is not Brickphyr")
            return identity, "running"
        if device.idProduct in (0x0008, 0x0011):
            return identity, "dfu"
        raise RuntimeError("Expected running Brickphyr or SPIKE/Inventor in DFU mode")
    finally:
        for device in devices:
            usb.util.dispose_resources(device)


def wait_for(identity, mode):
    deadline = time.monotonic() + 15
    first_error = None
    while True:
        try:
            state = attached()
        except usb.core.USBError as error:
            if not hub_usb.is_reset_error(error):
                raise
            first_error = first_error or error
            state = None
        if state is not None:
            if state[0] != identity:
                raise RuntimeError("A different hub appeared; keep the same USB connection")
            if state[1] == mode:
                return
        if time.monotonic() >= deadline:
            raise TimeoutError(f"Hub did not enter {mode} mode within 15 seconds") from first_error
        time.sleep(0.1)


async def info(client, expected_version=None):
    reply = await checked(client, Execute(argv=["info"]))
    if reply.ret or reply.truncated:
        raise RuntimeError(f"Firmware info failed: {reply.ret}; {reply.o}")
    version = re.search(r"^Brickphyr (\S+) \(([^\r\n]+)\)", reply.o, re.M)
    if not version or "Target spike_prime_f4/stm32f413xx" not in reply.o:
        raise RuntimeError(f"Unexpected firmware identity: {reply.o}")
    if expected_version is not None and version[1] != expected_version:
        raise RuntimeError(f"Expected firmware {expected_version}, found {version[1]}")
    return reply.o.strip()


async def enter_dfu():
    output = bytearray()

    def receive(data):
        output.extend(data[:max(0, 4096 - len(output))])

    async with console.connection(receive, management=True) as (transport, client):
        await info(client)
        try:
            await checked(client, application.Status())
        except Rejected as error:
            # The base profile has no application engine to stop. Only the
            # standard unsupported-group response establishes that condition.
            if error.group is not None or error.code != MGMT_ERR.ENOTSUP:
                raise
        else:
            print("Stopping application.", flush=True)
            await checked(client, application.Stop())
            state = await checked(client, application.Status())
            if state.state == 1 or state.cleanup:
                raise RuntimeError(
                    f"Application is not quiescent: state={state.state}, cleanup={state.cleanup}"
                )
        print("Entering USB DFU.", flush=True)
        # Successful reset cannot acknowledge a Shell/SMP request. Send once;
        # only subsequent DFU enumeration is evidence of a successful handoff.
        await transport.write(b"reboot dfu\r")
        try:
            await asyncio.wait_for(transport.closed.wait(), 10)
        except TimeoutError as error:
            raise RuntimeError(
                "DFU request did not close USB: " + output.decode(errors="replace")
            ) from error


async def verify(version):
    async with console.connection(lambda _: None, management=True) as (_, client):
        return await info(client, version)


def run(args):
    source = Path(args.firmware).resolve()
    checkout = (Path(args.pybricks) if args.pybricks else
                Path(__file__).resolve().parents[2] / "pybricks-micropython")
    installer = (checkout / "tools/flash").resolve()
    if not (installer / "__main__.py").is_file():
        raise ValueError("Pybricks installer not found; pass --pybricks PATH to its source checkout")
    # Check imports before changing the hub, and freeze the exact ZIP passed to
    # the upstream reader/writer so a concurrent rebuild cannot replace it.
    subprocess.run([sys.executable, str(installer), "--help"], check=True,
                   stdout=subprocess.DEVNULL)
    with TemporaryDirectory(prefix="brickphyr-flash-") as directory:
        package = Path(directory) / "firmware.zip"
        shutil.copyfile(source, package)
        version = check_package(package)
        state = attached()
        if state is None:
            raise RuntimeError("No hub found; connect and turn on the hub over USB")
        identity, mode = state
        print(f"Validated Brickphyr {version} for SPIKE/Inventor F4.", flush=True)
        if mode == "running":
            asyncio.run(enter_dfu())
        wait_for(identity, "dfu")
        print("Installing with Pybricks.", flush=True)
        subprocess.run([sys.executable, str(installer), str(package)], check=True)
        wait_for(identity, "running")
        print(asyncio.run(verify(version)))
        print("Firmware installed; USB connection closed.")
