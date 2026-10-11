# SPDX-License-Identifier: Apache-2.0
"""The same Zephyr Shell over USB or BLE, optionally sharing telemetry."""

import asyncio
import codecs
import os
import signal
import sys

from . import console, monitor


class _OutputBoundary:
    """Track insertion boundaries, without executing or rewriting Shell output."""

    def __init__(self):
        self.state = "text"
        self.osc = False
        self.utf8 = codecs.getincrementaldecoder("utf-8")("replace")

    @property
    def safe(self):
        return self.state == "text" and not self.utf8.getstate()[0]

    def feed(self, data):
        self.utf8.decode(data)
        for byte in data:
            if byte in (0x18, 0x1A):  # CAN and SUB cancel a control sequence.
                self.state = "text"
            elif self.state == "string_escape":
                self.state = "text" if byte == ord("\\") else "string"
            elif self.state == "string":
                if byte == 0x1B:
                    self.state = "string_escape"
                elif byte == 7 and self.osc:
                    self.state = "text"
            elif byte == 0x1B:
                self.state = "escape"
            elif self.state == "escape":
                if byte == ord("["):
                    self.state = "csi"
                elif byte in b"]PX^_":
                    self.state = "string"
                    self.osc = byte == ord("]")
                elif 0x20 <= byte <= 0x2F:
                    self.state = "intermediate"
                elif 0x30 <= byte <= 0x7E:
                    self.state = "text"
            elif self.state == "csi" and 0x40 <= byte <= 0x7E:
                self.state = "text"
            elif self.state == "intermediate" and 0x30 <= byte <= 0x7E:
                self.state = "text"


async def terminal(args):
    import termios
    import tty

    if not sys.stdin.isatty() or not sys.stdout.isatty():
        raise RuntimeError("The interactive terminal requires a TTY")
    usb = args.usb or args.port or args.serial
    if usb and args.target:
        raise ValueError("Choose a BLE target or USB options, not both")
    if not args.monitor and (args.log or args.source or args.rate != 2):
        raise ValueError("Use --monitor with --log, --source or --rate")
    target = (
        {"port": args.port, "serial_number": args.serial}
        if usb
        else {"ble": args.target or "Brickphyr"}
    )
    loop = asyncio.get_running_loop()
    incoming = asyncio.Queue(maxsize=16)
    exit_requested = asyncio.Event()
    input_closed = False
    view = False
    pending_view = False
    boundary = _OutputBoundary()
    last_screen = "Waiting for observations…"
    input_error = None

    def show_monitor(show):
        nonlocal view
        view = show
        sys.stdout.write("\x1b[?1049h\x1b[?25l" + last_screen if view else "\x1b[?25h\x1b[?1049l")
        sys.stdout.flush()

    def display(data):
        nonlocal pending_view
        # Preserve the real Shell screen/cursor, including a partly edited line.
        # New Shell output returns to it immediately; do not buffer or emulate it.
        if view:
            show_monitor(False)
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        boundary.feed(data)
        if pending_view and boundary.safe:
            pending_view = False
            show_monitor(True)

    def read_input():
        nonlocal input_closed, input_error
        try:
            data = os.read(sys.stdin.fileno(), 256)
            if not data:
                input_closed = True
                loop.remove_reader(sys.stdin.fileno())
            incoming.put_nowait(data)
            if incoming.full():
                # Leave further input in the TTY until the sender has room.
                loop.remove_reader(sys.stdin.fileno())
        except Exception as error:
            input_closed = True
            input_error = error
            loop.remove_reader(sys.stdin.fileno())
            exit_requested.set()

    async def input_loop(transport):
        nonlocal pending_view
        while data := await incoming.get():
            if not input_closed and incoming.qsize() == incoming.maxsize - 1:
                loop.add_reader(sys.stdin.fileno(), read_input)
            # Ctrl+] is handled by the TTY, independently of this sender.
            # Editing, history and completion go unchanged to the Shell.
            start = 0
            for index, byte in enumerate(data):
                if not args.monitor or byte != 0x14:
                    continue
                if not view:
                    await transport.write(data[start:index])
                if view:
                    show_monitor(False)
                elif pending_view:
                    pending_view = False
                elif boundary.safe:
                    show_monitor(True)
                else:
                    pending_view = True
                start = index + 1
            if not view:
                await transport.write(data[start:])

    with monitor.recording(args.log) as log:
        print(
            "Ctrl+] closes the terminal."
            + (" Ctrl+T switches the monitor view." if args.monitor else ""),
            flush=True,
        )
        previous = termios.tcgetattr(sys.stdin.fileno())
        previous_interrupt = signal.getsignal(signal.SIGINT)
        loop.add_signal_handler(signal.SIGINT, exit_requested.set)
        try:
            tty.setraw(sys.stdin.fileno())
            mode = termios.tcgetattr(sys.stdin.fileno())
            # The line discipline recognizes escape even while stdin is paused.
            # Flush unsent TTY input on exit so it cannot reach the parent shell.
            mode[3] = (mode[3] | termios.ISIG) & ~termios.NOFLSH
            mode[6][termios.VINTR] = b"\x1d"
            disabled = bytes([os.fpathconf(sys.stdin.fileno(), "PC_VDISABLE")])
            mode[6][termios.VQUIT] = mode[6][termios.VSUSP] = disabled
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, mode)
            async with console.connection(display, management=args.monitor, **target) as (
                transport,
                client,
            ):
                tasks = [
                    asyncio.create_task(input_loop(transport)),
                    asyncio.create_task(transport.closed.wait()),
                    asyncio.create_task(exit_requested.wait()),
                ]
                try:
                    if args.monitor:
                        identity, sources = await monitor.prepare(client, args.source)

                        def update(records, missed):
                            nonlocal last_screen
                            last_screen = monitor.screen(
                                monitor.render(identity, records, missed),
                                "Ctrl+T: terminal  |  Ctrl+]: close  |  monitoring continues in either view",
                            )
                            if view:
                                sys.stdout.write(last_screen)
                                sys.stdout.flush()

                        tasks.append(
                            asyncio.create_task(
                                monitor.watch(client, identity, sources, args.rate, log, update)
                            )
                        )
                    loop.add_reader(sys.stdin.fileno(), read_input)
                    done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                    if input_error:
                        raise RuntimeError(f"Terminal input failed: {input_error}") from input_error
                    for task in done:
                        task.result()
                    if transport.closed.is_set():
                        raise transport.error or RuntimeError("Console disconnected")
                finally:
                    loop.remove_reader(sys.stdin.fileno())
                    failure = sys.exception()
                    for task in tasks:
                        task.cancel(str(failure) if failure else None)
                    await asyncio.gather(*tasks, return_exceptions=True)
        finally:
            original = sys.exception()
            cleanup = None
            try:
                if view:
                    sys.stdout.write("\x1b[?25h\x1b[?1049l")
                    sys.stdout.flush()
            except Exception as error:
                cleanup = error
            try:
                termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, previous)
            except Exception as error:
                cleanup = cleanup or error
            try:
                loop.remove_signal_handler(signal.SIGINT)
                signal.signal(signal.SIGINT, previous_interrupt)
            except Exception as error:
                cleanup = cleanup or error
            try:
                print()
            except Exception as error:
                cleanup = cleanup or error
            if original is None and cleanup:
                raise cleanup


def run(args):
    asyncio.run(terminal(args))
