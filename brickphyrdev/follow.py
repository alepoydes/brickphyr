# SPDX-License-Identifier: Apache-2.0
"""Launch, output and cycle collection on one native management connection."""

import asyncio
import sys

from . import application as app
from . import console, recording
from .smp import checked


async def _output(client):
    dropped = 0
    while True:
        state = await checked(client, app.Stream())
        if state.data:
            sys.stdout.buffer.write(state.data)
            sys.stdout.buffer.flush()
        if state.dropped != dropped:
            print(f"[Application output dropped: {state.dropped} bytes]", file=sys.stderr)
            dropped = state.dropped
        if state.state != 1 and not state.queued:
            return state
        if not state.data:
            # Same 50 ms idle output-latency target as the interactive REPL.
            # Busy output runs at the acknowledged transport pace.
            await asyncio.sleep(0.05)


async def _records(client, log):
    with recording.session(log) as (session, counts):
        await recording.collect(client, log, session, counts, wait=True)
    print(f"[Recorded {counts['records']} frames; {counts['gaps']} gaps]", file=sys.stderr)


async def watch(client, log=None):
    tasks = [asyncio.create_task(_output(client))]
    if log is not None:
        tasks.append(asyncio.create_task(_records(client, log)))
    try:
        # ConsoleTransport serializes these public SMP requests on one stream.
        # Waiting tasks alternate at packet boundaries; neither opens a link.
        state, *_ = await asyncio.gather(*tasks)
    finally:
        for task in tasks:
            task.cancel()
        # Retain the first operation error, including external cancellation.
        await asyncio.gather(*tasks, return_exceptions=True)
    message = (f"Application ended: result {state.result}, fault {state.fault}, "
               f"cleanup {state.cleanup}, dropped {state.dropped}")
    if state.result or state.fault or state.cleanup:
        raise RuntimeError(message)
    print(f"[{message}]", file=sys.stderr)


async def execute(args, log):
    following = False
    try:
        async with console.connection(
            lambda data: None, management=True,
            ble=args.ble, port=args.port, serial_number=args.serial,
        ) as (transport, client):
            status = await checked(client, app.Status())
            if status.state == 1:
                raise RuntimeError("An application is running; stop it before launching another")
            # Attach before starting so a short program cannot fill its output queue
            # before the reader exists. Session closure detaches without sending STOP.
            if status.formats & (1 << app.Format.PYTHON):
                await checked(client, app.OutputStream(attached=True))
            await app.launch(client, transport, args)
            following = True
            await watch(client, log)
    except BaseException:
        if log is not None and not following:
            # Match standalone recording: retain failure even before metadata
            # exists, without marking a completed capture as failed afterward.
            with recording.session(log):
                raise
        raise


def run(args):
    with recording.open_log(args.record) as log:
        try:
            asyncio.run(execute(args, log))
        except KeyboardInterrupt:
            raise KeyboardInterrupt("Disconnected; no application stop was requested.") from None
