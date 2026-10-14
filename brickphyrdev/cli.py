# SPDX-License-Identifier: Apache-2.0
"""Connect to Brickphyr hubs and select their USB interface."""

import argparse

from . import application, compiler, monitor, terminal, usb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(required=True)
    compile_parser = commands.add_parser("compile", help="Compile Python source with the SDK")
    compile_parser.add_argument("source", help="Python source file")
    compile_parser.add_argument("--output", help="Output .mpy path; default is SOURCE.mpy")
    compile_parser.add_argument("--sdk", help="Extracted native SDK directory")
    compile_parser.set_defaults(handler=compiler.run)
    terminal_parser = commands.add_parser("terminal", help="Open the shared Shell over BLE or USB")
    terminal_parser.add_argument(
        "target", nargs="?", help="BLE name or address (default Brickphyr); omit for USB"
    )
    terminal_parser.add_argument("--usb", action="store_true", help="Select and open the USB Shell")
    terminal_parser.add_argument(
        "--repl", action="store_true", help="Start or reattach Python REPL; Ctrl+X returns to Shell"
    )
    terminal_usb = terminal_parser.add_mutually_exclusive_group()
    terminal_usb.add_argument("--port", help="Explicit serial port, including a simulator PTY")
    terminal_usb.add_argument("--serial", help="USB serial number when several hubs are connected")
    terminal_parser.add_argument(
        "--monitor", action="store_true", help="Share telemetry and logging; Ctrl+T switches views"
    )
    monitor.options(terminal_parser)
    terminal_parser.set_defaults(handler=terminal.run)

    monitor_parser = commands.add_parser(
        "monitor", help="Display read-only telemetry and optionally record JSONL"
    )
    target = monitor_parser.add_mutually_exclusive_group()
    target.add_argument(
        "--ble", metavar="TARGET", help="BLE name or address; default transport is USB"
    )
    target.add_argument("--port", help="Explicit serial port, including a simulator PTY")
    target.add_argument("--serial", help="USB serial number when several hubs are connected")
    monitor_parser.add_argument("--count", type=int, help="Exit after this many refresh cycles")
    monitor.options(monitor_parser)
    monitor_parser.set_defaults(handler=monitor.run)

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

    app_parser = commands.add_parser("app", help="Manage applications through SMP")
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
        transfer = app_commands.add_parser(command, help=help_text)
        transfer.add_argument(
            "image", help="Image file or Python directory containing main.py/main.mpy"
        )
        transfer.add_argument(
            "--format",
            choices=[format.name.lower() for format in application.Format],
            help="Default: Python for .py, MPY for .mpy, files for a directory/.cbor, ELF otherwise",
        )
    installation = app_commands.add_parser(
        "install", help="Store a validated application without starting"
    )
    installation.add_argument("image")
    installation.add_argument("--name", help="Installed name; default is the image filename stem")
    app_commands.add_parser(
        "start", help="Start an installed name or the current RAM upload"
    ).add_argument("name", nargs="?")
    app_commands.add_parser("remove", help="Remove an installed image").add_argument("name")
    for command in ("list", "status", "stop", "output", "discard", "interrupt"):
        app_commands.add_parser(command)
    app_commands.add_parser("record", help="Collect retained cycle records to JSONL").add_argument(
        "file", help="New JSONL file; existing files are never replaced"
    )
    app_commands.add_parser("record-clear", help="Clear retained records after the app stops")
    app_input = app_commands.add_parser("input", help="Send a line to the running Python program")
    app_input.add_argument("text")
    app_input.add_argument("--no-newline", action="store_true", help="Send text without a line ending")

    args = parser.parse_args()
    try:
        args.handler(args)
    except (Exception, KeyboardInterrupt) as error:
        parser.exit(1, f"{error}\n")
