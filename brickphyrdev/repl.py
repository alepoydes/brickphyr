# SPDX-License-Identifier: Apache-2.0
"""Interactive Python I/O using the existing application operations."""

import asyncio
import errno

from . import application as app
from .smp import Rejected, checked


class Session:
    def __init__(self, client, display):
        self.client, self.display = client, display
        self.closing = asyncio.Event()

    async def write(self, data):
        offset = 0
        while offset < len(data) and not self.closing.is_set():
            chunk = data[offset : offset + 128]
            try:
                await checked(self.client, app.Input(data=chunk))
            except Rejected as error:
                if error.group != app.GROUP or error.code != errno.EAGAIN:
                    raise
                # Retain rejected bytes. Closing remains available while the VM
                # does not read; the TTY frontend bounds additional keyboard input.
                await self._wait()
            else:
                offset += len(chunk)

    async def _wait(self):
        # 50 ms is the idle interactive-output latency target, not a device
        # sampling period. Busy output drains at the transport's acknowledged pace.
        try:
            await asyncio.wait_for(self.closing.wait(), 0.05)
        except TimeoutError:
            pass

    async def watch(self):
        dropped = 0
        while not self.closing.is_set():
            state = await checked(self.client, app.Stream())
            if state.data:
                self.display(state.data)
            if state.dropped != dropped:
                self.display(f"\r\n[Python output dropped: {state.dropped} bytes]\r\n".encode())
                dropped = state.dropped
            if (state.state != 1 or not state.repl) and not state.queued:
                self.display(
                    f"\r\n[REPL ended: result {state.result}, fault {state.fault}, "
                    f"cleanup {state.cleanup}]\r\n".encode()
                )
                return
            if not state.data:
                await self._wait()
        # Explicit exit is different from detaching the host connection. The
        # latter cancels this watcher and leaves the autonomous VM running.
        state = await checked(self.client, app.Status())
        if state.repl and state.state == 1:
            await checked(self.client, app.Stop())
