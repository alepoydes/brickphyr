# SPDX-License-Identifier: Apache-2.0
"""Open the hub's native BLE terminal; line editing runs entirely on the hub."""

import asyncio
import os
import sys
import termios
import tty

from bleak import BleakClient, BleakScanner

# Native Brickphyr stream; Pybricks' NUS belongs to application stdin/stdout.
SERVICE = "f3641400-00b0-4240-ba50-05ca45bf8abc"
TX = "f3641401-00b0-4240-ba50-05ca45bf8abc"
RX = "f3641402-00b0-4240-ba50-05ca45bf8abc"


async def terminal(target):
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        raise RuntimeError("The interactive terminal requires a TTY")
    devices = await BleakScanner.discover(timeout=5)
    matches = [d for d in devices if d.address.casefold() == target.casefold() or d.name == target]
    if len(matches) != 1:
        available = ", ".join(f"{d.name or '?'} ({d.address})" for d in matches)
        raise RuntimeError(f"Expected one {target!r} hub, found {len(matches)}: {available}")

    loop = asyncio.get_running_loop()
    stopped = asyncio.Event()
    incoming = asyncio.Queue(maxsize=16)
    input_error = None
    exiting = False

    def read_input():
        nonlocal input_error
        data = os.read(sys.stdin.fileno(), 256)
        if not data:
            stopped.set()
            return
        try:
            incoming.put_nowait(data)
        except asyncio.QueueFull:
            input_error = RuntimeError("Terminal input exceeded BLE throughput; session closed")
            stopped.set()

    def output(_characteristic, data):
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()

    async with BleakClient(matches[0], disconnected_callback=lambda _: stopped.set()) as client:
        if client.services.get_service(SERVICE) is None:
            raise RuntimeError("This hub does not provide the native Brickphyr terminal")
        print("Connected. Ctrl+] closes the terminal.", flush=True)
        previous = termios.tcgetattr(sys.stdin.fileno())
        tty.setraw(sys.stdin.fileno())
        try:
            # Install output handling before subscribing: subscription starts the shared Shell.
            await client.start_notify(TX, output)
            loop.add_reader(sys.stdin.fileno(), read_input)
            stop = asyncio.create_task(stopped.wait())
            try:
                while not stopped.is_set():
                    read = asyncio.create_task(incoming.get())
                    try:
                        done, _ = await asyncio.wait(
                            (read, stop), return_when=asyncio.FIRST_COMPLETED
                        )
                        if stop in done:
                            break
                        data = read.result()
                    finally:
                        read.cancel()
                        await asyncio.gather(read, return_exceptions=True)
                    data, separator, _ = data.partition(b"\x1d")
                    # 20 bytes work even with the minimum ATT MTU; acknowledged writes
                    # let the hub reject a whole chunk before accepting any of it.
                    for offset in range(0, len(data), 20):
                        await asyncio.wait_for(
                            client.write_gatt_char(RX, data[offset : offset + 20], response=True),
                            timeout=5,
                        )
                    if separator:
                        exiting = True
                        break
            finally:
                stop.cancel()
                await asyncio.gather(stop, return_exceptions=True)
        finally:
            loop.remove_reader(sys.stdin.fileno())
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, previous)
            print()
        if input_error:
            raise input_error
        if not exiting:
            raise RuntimeError("BLE terminal disconnected")


def run(args):
    asyncio.run(terminal(args.target))
