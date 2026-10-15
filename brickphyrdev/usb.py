# SPDX-License-Identifier: Apache-2.0
"""Select the experimental USB interface without detaching the CDC driver."""

import asyncio
import contextlib
import errno
import sys
import time

import serial
import serial.tools.list_ports

REQUEST = 0x42
WAKE_REQUEST = 0x43
MODES = ("pybricks", "shell")
WAKE_PHASES = ("running", "stopping", "off", "waking")


def device(serial_number=None, *, missing_ok=False):
    import usb.core
    import usb.util

    hubs = list(usb.core.find(find_all=True, idVendor=0x2FE3, idProduct=0x0004))
    selected = None
    try:
        matches = []
        for hub in hubs:
            # Device.langids hides USBError as an empty language list. Read
            # descriptors through the public functions so wake can distinguish
            # a reset race from denied access or an invalid device identity.
            languages = usb.util.get_langids(hub)
            if not languages:
                continue
            language = languages[0]
            if usb.util.get_string(hub, hub.iManufacturer, language) != "Brickphyr":
                continue
            if serial_number is not None and (
                usb.util.get_string(hub, hub.iSerialNumber, language) != serial_number
            ):
                continue
            matches.append(hub)
        if len(matches) == 1:
            selected = matches[0]
            return selected
        if missing_ok and not matches:
            return None
        raise RuntimeError(f"Expected one Brickphyr USB device, found {len(matches)}; use --serial")
    finally:
        for hub in hubs:
            if hub is not selected:
                with contextlib.suppress(Exception):
                    usb.util.dispose_resources(hub)


def status(hub):
    value = bytes(hub.ctrl_transfer(0xC0, REQUEST, 0, 0, 8, timeout=1000))
    if len(value) != 8 or value[0] != 1 or value[1] >= len(MODES):
        raise RuntimeError(f"Unknown Brickphyr selector response: {value.hex()}")
    error = int.from_bytes(value[4:], "little", signed=True)
    if error:
        raise RuntimeError(f"Hub transport failed: {error}; reboot before retrying")
    return MODES[value[1]], bool(value[2]), bool(value[3])


def _wake_phase(hub, timeout_ms):
    value = bytes(hub.ctrl_transfer(0xC0, WAKE_REQUEST, 0, 0, 2, timeout=timeout_ms))
    if len(value) != 2 or value[0] != 1 or value[1] >= len(WAKE_PHASES):
        raise RuntimeError(f"Unknown Brickphyr wake response: {value.hex()}")
    return WAKE_PHASES[value[1]]


def _reset_error(error):
    return getattr(error, "errno", None) in {
        errno.EIO, errno.ENODEV, errno.ECONNRESET, errno.ESHUTDOWN, errno.ETIMEDOUT
    } or getattr(error, "backend_error_code", None) in {-1, -4, -7}


def _serial_number(hub):
    import usb.util

    # Read directly here too: Device.serial_number can hide a reset while
    # fetching its language list. Never rediscover a different hub on failure.
    languages = usb.util.get_langids(hub)
    value = usb.util.get_string(hub, hub.iSerialNumber, languages[0]) if languages else None
    if not value:
        raise RuntimeError("Hub has no USB serial number")
    return value


def wake(hub):
    import usb.core
    import usb.util

    serial_number = _serial_number(hub)
    deadline = time.monotonic() + 10
    current = hub
    sent = False
    first_error = None
    while True:
        remaining_ms = int((deadline - time.monotonic()) * 1000)
        if remaining_ms <= 0:
            raise TimeoutError("Hub did not return to USB running state within 10 seconds") from first_error
        transfer_timeout = min(1000, remaining_ms)
        try:
            if current is None:
                current = device(serial_number, missing_ok=True)
            if current is not None:
                try:
                    phase = _wake_phase(current, transfer_timeout)
                    if phase == "running":
                        return
                    if phase == "off" and not sent:
                        # A reset may discard the status-stage ACK. Never send
                        # another SET; a later GET must prove the hub is running.
                        set_timeout = min(1000, int((deadline - time.monotonic()) * 1000))
                        if set_timeout <= 0:
                            raise TimeoutError("Hub did not return to USB running state within 10 seconds") from first_error
                        sent = True
                        current.ctrl_transfer(0x40, WAKE_REQUEST, 0, 0, b"", timeout=set_timeout)
                finally:
                    if current is not hub:
                        with contextlib.suppress(Exception):
                            usb.util.dispose_resources(current)
        except usb.core.USBError as error:
            if not _reset_error(error):
                raise
            if first_error is None:
                first_error = error
        finally:
            current = None
        time.sleep(min(0.05, max(0, deadline - time.monotonic())))


def serial_port(hub):
    serial_number = _serial_number(hub)
    ports = [
        port.device
        for port in serial.tools.list_ports.comports()
        if (port.vid, port.pid, port.serial_number)
        == (hub.idVendor, hub.idProduct, serial_number)
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


@contextlib.asynccontextmanager
async def _pybricks_connection(port):
    try:
        from pybricksdev.connections.serial import PybricksHubSerial
    except ImportError as error:
        raise RuntimeError(
            "USB requires the Brickphyr pybricksdev CDC extension; see docs/upstream.md"
        ) from error
    client = PybricksHubSerial(port)
    try:
        await client.connect()
        yield client
    except BaseException:
        # Report the failed operation even if releasing the port also fails.
        with contextlib.suppress(Exception):
            await client.disconnect()
        raise
    else:
        await client.disconnect()


def identify(hub):
    selected, occupied, ready = status(hub)
    if selected != "pybricks" or occupied or not ready:
        raise RuntimeError("Select pybricks and close other clients before identification")

    async def read():
        from pybricksdev.ble.pybricks import (
            DEVICE_NAME_UUID,
            FW_REV_UUID,
            PNP_ID_UUID,
            PYBRICKS_HUB_CAPABILITIES_UUID,
            SW_REV_UUID,
        )

        async with _pybricks_connection(serial_port(hub)) as client:
            for name, characteristic in (
                ("name", DEVICE_NAME_UUID),
                ("firmware", FW_REV_UUID),
                ("protocol", SW_REV_UUID),
                ("pnp", PNP_ID_UUID),
                ("capabilities", PYBRICKS_HUB_CAPABILITIES_UUID),
            ):
                value = await client.read_gatt_char(characteristic)
                print(
                    f"{name}: {value.hex() if name in ('pnp', 'capabilities') else value.decode()}"
                )

    asyncio.run(read())


def handover(hub, mode):
    current, occupied, ready = status(hub)
    if occupied or not ready:
        raise RuntimeError("Close other clients before requesting a handover")
    if current == mode:
        return
    if current == "shell":
        with serial.Serial(
            port=None, baudrate=115200, timeout=2, write_timeout=2, exclusive=True
        ) as port:
            # pySerial clears RX during open, after setting line controls. Start
            # the Shell only afterward so its first prompt cannot be discarded.
            port.dtr = False
            port.port = serial_port(hub)
            port.open()
            port.dtr = True
            if not port.read_until(b"brickphyr> ").endswith(b"brickphyr> "):
                raise TimeoutError("Shell did not become ready")
            port.write(b"host_mode pybricks\r")
            reply = port.read_until(b"brickphyr> ")
            if b"Pybricks selected for the next session." not in reply:
                raise RuntimeError(f"Handover rejected: {reply!r}")
            if status(hub)[0] != current:
                raise RuntimeError("Interface changed before its owner closed the session")
    else:

        async def leave():
            from pybricksdev.ble.pybricks import PYBRICKS_COMMAND_EVENT_UUID

            async with _pybricks_connection(serial_port(hub)) as client:
                # Brickphyr-specific EXIT_COMPATIBILITY; the client correlates the reply.
                await client.write_gatt_char(PYBRICKS_COMMAND_EVENT_UUID, b"\x80", True)
                if status(hub)[0] != current:
                    raise RuntimeError("Interface changed before its owner closed the session")

        asyncio.run(leave())
    deadline = time.monotonic() + 2
    while status(hub) != (mode, False, True):
        if time.monotonic() >= deadline:
            raise TimeoutError("Requested interface did not become ready after close")
        time.sleep(0.02)


def run(args):
    import usb.util

    hub = None
    try:
        hub = device(args.serial)
        if args.command == "select":
            select(hub, args.mode)
        elif args.command == "handover":
            handover(hub, args.mode)
        elif args.command == "identify":
            identify(hub)
            return
        elif args.command == "wake":
            wake(hub)
            print("USB running")
            return
        mode, occupied, ready = status(hub)
        print(
            f"{mode}: {'occupied' if occupied else 'idle'}, {'selectable' if ready else 'selection blocked'}"
        )
    finally:
        if hub is not None:
            if sys.exception() is not None or args.command == "wake":
                with contextlib.suppress(Exception):
                    usb.util.dispose_resources(hub)
            else:
                usb.util.dispose_resources(hub)
