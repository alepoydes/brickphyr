# SPDX-License-Identifier: Apache-2.0
"""Connect to Brickphyr hubs and select their USB interface."""

import argparse

from . import application, terminal, usb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(required=True)
    terminal_parser = commands.add_parser("terminal", help="Open the native Bluetooth terminal")
    terminal_parser.add_argument(
        "target", nargs="?", default="Brickphyr", help="Hub name or BLE address"
    )
    terminal_parser.set_defaults(handler=terminal.run)

    usb_parser = commands.add_parser("usb", help="Inspect or select the USB interface")
    usb_parser.add_argument("--serial", help="USB serial number when several hubs are connected")
    usb_parser.set_defaults(handler=usb.run)
    usb_commands = usb_parser.add_subparsers(dest="command", required=True)
    usb_commands.add_parser("status", help="Show USB mode and session ownership")
    selection = usb_commands.add_parser("select", help="Select through USB control requests")
    selection.add_argument("mode", choices=usb.MODES)
    switching = usb_commands.add_parser("handover", help="Switch through the current protocol")
    switching.add_argument("mode", choices=usb.MODES)
    usb_commands.add_parser("identify", help="Read identity through the Pybricks client")

    app_parser = commands.add_parser("app", help="Manage native applications through SMP")
    target = app_parser.add_mutually_exclusive_group()
    target.add_argument(
        "--ble", metavar="TARGET", help="BLE name or address; default transport is USB"
    )
    target.add_argument("--port", help="Explicit serial port (also suitable for a simulator)")
    app_parser.add_argument("--serial", help="USB serial number when several hubs are connected")
    app_parser.set_defaults(handler=application.run)
    app_commands = app_parser.add_subparsers(dest="command", required=True)
    for command, help_text in (
        ("run", "Upload to RAM and start"),
        ("upload", "Upload to RAM without starting"),
    ):
        app_commands.add_parser(command, help=help_text).add_argument("image")
    for command in ("start", "status", "stop", "output", "discard"):
        app_commands.add_parser(command)

    args = parser.parse_args()
    try:
        args.handler(args)
    except (Exception, KeyboardInterrupt) as error:
        parser.exit(1, f"{error}\n")
