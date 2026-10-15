# SPDX-License-Identifier: Apache-2.0
"""The same Zephyr Shell over USB or BLE, optionally sharing telemetry."""

import asyncio
import codecs
import os
import signal
import sys

from . import application, console, monitor, recording, repl


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
    record_path = getattr(args, "record", None)
    has_view = args.monitor or bool(record_path)
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
    last_body = ["Waiting for observations…"] if args.monitor else []
    last_screen = ""
    record_state = "Waiting for a recording"
    record_counts = {"records": 0, "gaps": 0}
    input_error = None
    input_fd = None
    python = None
    use_repl = getattr(args, "repl", False)

    def refresh_screen():
        nonlocal last_screen
        body = list(last_body)
        if record_path:
            body += [f"Recording: {record_path}", f"{record_state}; "
                     f"{record_counts['records']} records, {record_counts['gaps']} gaps"]
        last_screen = monitor.screen(body,
            "Ctrl+T: terminal  |  Ctrl+]: close  |  collection continues in either view")
        if view:
            sys.stdout.write(last_screen)
            sys.stdout.flush()

    refresh_screen()

    def discard_input():
        loop.remove_reader(input_fd)
        while not incoming.empty():
            incoming.get_nowait()

    def leave_repl():
        if python is not None:
            python.closing.set()
            discard_input()

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
            data = os.read(input_fd, 256)
            if not data:
                input_closed = True
                loop.remove_reader(input_fd)
            incoming.put_nowait(data)
            if incoming.full():
                # Leave further input in the TTY until the sender has room.
                loop.remove_reader(input_fd)
        except BlockingIOError:
            # A signal can flush TTY input after the readiness notification.
            pass
        except Exception as error:
            input_closed = True
            input_error = error
            loop.remove_reader(input_fd)
            exit_requested.set()

    async def input_loop(transport):
        nonlocal pending_view
        while data := await incoming.get():
            if not input_closed and incoming.qsize() == incoming.maxsize - 1:
                loop.add_reader(input_fd, read_input)
            # Ctrl+] is handled by the TTY, independently of this sender.
            # Editing, history and completion go unchanged to the active parser.
            send = python.write if python is not None else transport.write
            start = 0
            for index, byte in enumerate(data):
                if not has_view or byte != 0x14:
                    continue
                if not view:
                    await send(data[start:index])
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
                await send(data[start:])

    with monitor.recording(args.log) as log, recording.open_log(record_path, "cbor") as record_log:
        print(
            "Ctrl+] closes the terminal."
            + (" Ctrl+X stops REPL and returns to Shell; Ctrl+D resets Python." if use_repl else "")
            + (" Ctrl+T switches the status view." if has_view else "")
            + (f" Recording to {record_path}." if record_path else ""),
            flush=True,
        )
        previous = termios.tcgetattr(sys.stdin.fileno())
        previous_interrupt = signal.getsignal(signal.SIGINT)
        previous_quit = signal.getsignal(signal.SIGQUIT)
        loop.add_signal_handler(signal.SIGINT, exit_requested.set)
        if use_repl:
            loop.add_signal_handler(signal.SIGQUIT, leave_repl)
        try:
            # Readiness may be stale after Ctrl+]/Ctrl+X flush the line discipline.
            # Open independently: dup/set_blocking(stdin) would also change stdout
            # when the two descriptors share an open file description.
            input_fd = os.open(
                os.ttyname(sys.stdin.fileno()), os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY
            )
            tty.setraw(sys.stdin.fileno())
            mode = termios.tcgetattr(sys.stdin.fileno())
            # The line discipline recognizes escape even while stdin is paused.
            # Flush unsent TTY input on exit so it cannot reach the parent shell.
            mode[3] = (mode[3] | termios.ISIG) & ~termios.NOFLSH
            mode[6][termios.VINTR] = b"\x1d"
            disabled = bytes([os.fpathconf(sys.stdin.fileno(), "PC_VDISABLE")])
            mode[6][termios.VQUIT] = mode[6][termios.VSUSP] = disabled
            if use_repl:
                # Like detach, explicit REPL exit must work even when queued
                # keyboard input is backpressured by a program that never reads.
                mode[6][termios.VQUIT] = b"\x18"
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, mode)
            async with console.connection(
                display, management=has_view or use_repl, **target
            ) as (
                transport,
                client,
            ):
                if use_repl:
                    await application.checked(client, application.OutputStream(attached=True))
                    await application.checked(client, application.Repl())
                    python = repl.Session(client, display)

                async def python_loop():
                    nonlocal python
                    await python.watch()
                    await application.checked(client, application.OutputStream(attached=False))
                    leave_repl()
                    python = None
                    mode[6][termios.VQUIT] = disabled
                    termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, mode)
                    display(b"\r\n")
                    await transport.write(b"\x03")
                    if not input_closed:
                        loop.add_reader(input_fd, read_input)

                async def record_loop():
                    nonlocal record_state, record_counts
                    # A terminal may open before the user prepares capture.
                    # App follow's wait semantics still end when its app exits.
                    while not (await recording.checked(client, recording.Info())).size:
                        await asyncio.sleep(0.2)
                    record_state = "Receiving"
                    with recording.session(record_log) as (session, counts):
                        record_counts = counts
                        await recording.collect(client, record_log, session, counts)
                    record_state = "Download complete; hub data retained"
                    refresh_screen()

                async def record_view():
                    while True:
                        refresh_screen()
                        await asyncio.sleep(1 / args.rate)

                tasks = [
                    asyncio.create_task(input_loop(transport)),
                    asyncio.create_task(transport.closed.wait()),
                    asyncio.create_task(exit_requested.wait()),
                ]
                python_task = asyncio.create_task(python_loop()) if use_repl else None
                if python_task is not None:
                    tasks.append(python_task)
                record_task = None
                try:
                    if record_path:
                        record_task = asyncio.create_task(record_loop())
                        tasks.append(record_task)
                        if not args.monitor:
                            tasks.append(asyncio.create_task(record_view()))
                    if args.monitor:
                        identity, sources = await monitor.prepare(client, args.source)

                        def update(records, missed):
                            nonlocal last_body
                            last_body = monitor.render(identity, records, missed)
                            refresh_screen()

                        tasks.append(
                            asyncio.create_task(
                                monitor.watch(client, identity, sources, args.rate, log, update)
                            )
                        )
                    loop.add_reader(input_fd, read_input)
                    while True:
                        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                        if input_error:
                            raise RuntimeError(
                                f"Terminal input failed: {input_error}"
                            ) from input_error
                        for task in done:
                            task.result()
                        # A finished download or REPL leaves the Shell open.
                        for task in (python_task, record_task):
                            if task in done:
                                tasks.remove(task)
                                done.remove(task)
                        if done:
                            break
                    if transport.closed.is_set():
                        raise transport.error or RuntimeError("Console disconnected")
                finally:
                    loop.remove_reader(input_fd)
                    failure = sys.exception()
                    for task in tasks:
                        task.cancel(str(failure) if failure else None)
                    await asyncio.gather(*tasks, return_exceptions=True)
        finally:
            original = sys.exception()
            cleanup = None
            if input_fd is not None:
                try:
                    loop.remove_reader(input_fd)
                    os.close(input_fd)
                except Exception as error:
                    cleanup = error
            try:
                if view:
                    sys.stdout.write("\x1b[?25h\x1b[?1049l")
                    sys.stdout.flush()
            except Exception as error:
                cleanup = cleanup or error
            try:
                termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, previous)
            except Exception as error:
                cleanup = cleanup or error
            try:
                loop.remove_signal_handler(signal.SIGINT)
                signal.signal(signal.SIGINT, previous_interrupt)
                if use_repl:
                    loop.remove_signal_handler(signal.SIGQUIT)
                    signal.signal(signal.SIGQUIT, previous_quit)
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
