# SPDX-License-Identifier: Apache-2.0
"""Select the experimental USB interface without detaching the CDC driver."""

import argparse
import sys
import time
from pathlib import Path

import serial
import serial.tools.list_ports
from west.manifest import Manifest

REQUEST = 0x42
MODES = ("pybricks", "shell")


def device(serial_number=None):
    import usb.core

    hubs = [
        hub
        for hub in usb.core.find(find_all=True, idVendor=0x2FE3, idProduct=0x0004)
        if hub.manufacturer == "Brickphyr"
        and (serial_number is None or hub.serial_number == serial_number)
    ]
    if len(hubs) != 1:
        raise RuntimeError(f"Expected one Brickphyr USB device, found {len(hubs)}; use --serial")
    return hubs[0]


def status(hub):
    value = bytes(hub.ctrl_transfer(0xC0, REQUEST, 0, 0, 8, timeout=1000))
    if len(value) != 8 or value[0] != 1 or value[1] >= len(MODES):
        raise RuntimeError(f"Unknown Brickphyr selector response: {value.hex()}")
    error = int.from_bytes(value[4:], "little", signed=True)
    if error:
        raise RuntimeError(f"Hub transport failed: {error}; reboot before retrying")
    return MODES[value[1]], bool(value[2]), bool(value[3])


def serial_port(hub):
    ports = [
        port.device
        for port in serial.tools.list_ports.comports()
        if (port.vid, port.pid, port.serial_number)
        == (hub.idVendor, hub.idProduct, hub.serial_number)
    ]
    if len(ports) != 1:
        raise RuntimeError(f"Expected one matching CDC port, found {len(ports)}")
    return ports[0]


def select(hub, mode):
    if status(hub)[1]:
        raise RuntimeError("USB session is occupied; close the terminal/client first")
    # DTR stays low: drain already queued USB output without starting a session.
    with serial.Serial(port=None, timeout=0, exclusive=True) as drain:
        drain.dtr = False
        drain.port = serial_port(hub)
        drain.open()
        deadline = time.monotonic() + 2
        while True:
            drain.read(4096)
            current, occupied, ready = status(hub)
            if occupied:
                raise RuntimeError("Another client acquired the USB session")
            if ready:
                break
            if time.monotonic() >= deadline:
                raise TimeoutError("Previous USB session has not finished draining")
            time.sleep(0.02)
        drain.reset_input_buffer()
        # Device recipient: libusb must not claim or detach the CDC interfaces.
        hub.ctrl_transfer(0x40, REQUEST, MODES.index(mode), 0, b"", timeout=1000)
        if status(hub)[0] != mode:
            raise RuntimeError(f"Hub did not select {mode}")


def protocol_client():
    project = Manifest.from_topdir().get_projects(["pybricks"])[0]
    sys.path.insert(0, str(Path(project.abspath) / "tools/flash"))
    from serdev import pybricks

    return pybricks


def identify(hub):
    selected, occupied, ready = status(hub)
    if selected != "pybricks" or occupied or not ready:
        raise RuntimeError("Select pybricks and close other clients before identification")
    client = protocol_client()
    with serial.Serial(
        serial_port(hub), 115200, timeout=1, write_timeout=1, exclusive=True
    ) as port:
        port.reset_input_buffer()
        for name, service, characteristic in (
            ("name", 1, 0x2A00),
            ("firmware", 1, 0x2A26),
            ("protocol", 1, 0x2A28),
            ("pnp", 1, 0x2A50),
            ("capabilities", 2, 3),
        ):
            client.send_read(port, service, characteristic)
            reply = client.read_frame(port, timeout=1)
            prefix = bytes((3, service)) + characteristic.to_bytes(2, "little")
            if reply is None or not reply.startswith(prefix):
                raise RuntimeError(f"Invalid {name} response: {reply!r}")
            value = reply[4:]
            print(f"{name}: {value.hex() if name in ('pnp', 'capabilities') else value.decode()}")


def main():
    import usb.core
    import usb.util

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="USB serial number when several hubs are connected")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    selection = commands.add_parser("select")
    selection.add_argument("mode", choices=MODES)
    commands.add_parser("identify", help="Read identity using the unmodified Pybricks client")
    args = parser.parse_args()
    hub = None
    try:
        hub = device(args.serial)
        if args.command == "select":
            select(hub, args.mode)
        elif args.command == "identify":
            identify(hub)
            return
        mode, occupied, ready = status(hub)
        print(
            f"{mode}: {'occupied' if occupied else 'idle'}, {'selectable' if ready else 'selection blocked'}"
        )
    except (RuntimeError, ValueError, TimeoutError, usb.core.USBError, serial.SerialException) as error:
        parser.exit(1, f"{error}\n")
    finally:
        if hub is not None:
            usb.util.dispose_resources(hub)


if __name__ == "__main__":
    main()
