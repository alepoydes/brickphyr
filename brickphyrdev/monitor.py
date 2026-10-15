# SPDX-License-Identifier: Apache-2.0
"""Basic text telemetry and JSONL recording on the computer."""

import argparse
import asyncio
import contextlib
import datetime
import json
import math
import shutil
import sys
import time
import uuid

from . import telemetry
from .smp import connection


def rate(value):
    value = float(value)
    if not math.isfinite(value) or not 0 < value <= 20:
        raise argparse.ArgumentTypeError("Display rate must be greater than 0 and at most 20 Hz")
    return value


def options(parser):
    parser.add_argument("--source", action="append", help="Source name; repeat to select several")
    parser.add_argument(
        "--rate", type=rate, default=2.0, help="Maximum display refresh rate, 0–20 Hz (default 2)"
    )
    parser.add_argument(
        "--log", metavar="FILE", help="Create a JSONL telemetry log on this computer"
    )


@contextlib.contextmanager
def recording(path, *, binary=False, resume=False):
    # Refuse replacement before acquiring a hub connection.
    mode = "r+" if resume else "x"
    stream = (open(path, mode + "b") if binary else open(path, mode, encoding="utf-8")) if path else None
    try:
        yield stream
    except BaseException:
        if stream:
            with contextlib.suppress(Exception):
                stream.close()
        raise
    else:
        if stream:
            stream.close()


def write_log(stream, value):
    if stream:
        stream.write(json.dumps(value, ensure_ascii=True, separators=(",", ":")) + "\n")
        stream.flush()


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


async def prepare(client, selection):
    identity = await telemetry.info(client)
    sources = selection or identity.sources
    unknown = set(sources) - set(identity.sources)
    if unknown:
        raise ValueError(
            f"Unavailable sources: {', '.join(sorted(unknown))}; available: {', '.join(identity.sources)}"
        )
    if len(set(sources)) != len(sources):
        raise ValueError("Select each source only once")
    if not sources:
        raise ValueError("This firmware provides no telemetry sources")
    return identity, sources


def snapshot_record(value):
    return {
        "type": "snapshot",
        "received_utc": utc_now(),
        "received_monotonic_ns": time.monotonic_ns(),
        "source": value.source,
        "read_ms": value.read_ms,
        "error": value.error,
        "values": {k: list(v) if isinstance(v, bytes) else v for k, v in value.values.items()},
    }


async def watch(client, identity, sources, hz, log, update, *, count=None):
    session = str(uuid.uuid4())
    write_log(
        log,
        {
            "type": "session",
            "schema": "brickphyr.telemetry/1",
            "session": session,
            "started_utc": utc_now(),
            "firmware": identity.firmware,
            "build": identity.build,
            "board": identity.board,
            "sources": sources,
            "rate_hz": hz,
        },
    )
    latest = {}
    missed = cycles = records = 0
    failure = None
    loop = asyncio.get_running_loop()
    period = 1 / hz
    deadline = loop.time()
    try:
        while count is None or cycles < count:
            for source in sources:
                record = snapshot_record(await telemetry.snapshot(client, source))
                previous = latest.get(source)
                if previous and record["read_ms"] < previous["read_ms"]:
                    raise RuntimeError(
                        "Hub uptime moved backwards; reconnect to start a new recording"
                    )
                write_log(log, record)
                latest[source] = record
                records += 1
            cycles += 1
            update(latest, missed)
            deadline += period
            now = loop.time()
            if deadline <= now:
                skipped = math.floor((now - deadline) / period) + 1
                missed += skipped
                deadline += skipped * period
            if count is None or cycles < count:
                await asyncio.sleep(max(0, deadline - loop.time()))
    except BaseException as error:
        failure = error
        raise
    finally:
        ending = {
            "type": "end",
            "session": session,
            "ended_utc": utc_now(),
            "reason": "cancelled"
            if isinstance(failure, asyncio.CancelledError)
            else "error"
            if failure
            else "completed",
            "records": records,
            "cycles": cycles,
            "missed_polls": missed,
        }
        if failure and str(failure):
            ending["error"] = str(failure)
        if failure:
            with contextlib.suppress(Exception):
                write_log(log, ending)
        else:
            write_log(log, ending)


def clean(value):
    return "".join(c if c.isprintable() else "?" for c in str(value))


def render(identity, records, missed):
    lines = [
        f"Brickphyr {clean(identity.firmware)}  |  {clean(identity.board)}  |  missed polls: {missed}"
    ]
    now = time.monotonic_ns()
    detailed = len(records) == 1
    for name, record in records.items():
        values = record["values"]
        elapsed = max(0, (now - record["received_monotonic_ns"]) // 1_000_000)
        status = f"unavailable ({record['error']})" if record["error"] else "ok"
        sample_ms = values.get("sample_ms", values.get("sample_us", 0) // 1000)
        if sample_ms:
            age = max(0, record["read_ms"] - sample_ms) + elapsed
            status += f"; sample age {age} ms"
        else:
            status += f"; read age {elapsed} ms"
        lines.append(f"{clean(name)}: {status}")
        if not detailed:
            summary = "  " + " ".join(
                f"{clean(k)}={v}"
                for k, v in values.items()
                if k != "sample_ms" and not isinstance(v, list)
            )
            lines[-1] += " | " + summary.strip()
            continue
        for key, value in values.items():
            if key == "sample_ms":
                continue
            if isinstance(value, list):
                lines.append(f"  {clean(key)}:")
                for start in range(0, len(value), 5):
                    lines.append("    " + " ".join(f"{v:3}" for v in value[start : start + 5]))
            else:
                lines.append(f"  {clean(key)}={value}")
    return lines


def screen(lines, hint):
    columns, rows = shutil.get_terminal_size((80, 24))
    visible = max(1, rows - 2)
    if len(lines) > visible:
        lines = lines[: max(0, visible - 1)] + ["… more fields; use --source to narrow the view"]
    return "\x1b[H\x1b[2J" + "\r\n".join(x[: max(1, columns - 1)] for x in [*lines, "", hint])


async def execute(args):
    with recording(args.log) as log:
        async with connection(ble=args.ble, port=args.port, serial_number=args.serial) as (
            client,
            _,
        ):
            identity, sources = await prepare(client, args.source)
            interactive = sys.stdout.isatty()
            if interactive:
                print("\x1b[?1049h\x1b[?25l", end="", flush=True)
            try:

                def update(records, missed):
                    lines = render(identity, records, missed)
                    print(
                        screen(lines, "Ctrl+C closes the monitor.")
                        if interactive
                        else "\n".join(lines),
                        end="" if interactive else "\n\n",
                        flush=True,
                    )

                await watch(client, identity, sources, args.rate, log, update, count=args.count)
            finally:
                if interactive:
                    print("\x1b[?25h\x1b[?1049l", end="", flush=True)


def run(args):
    if args.count is not None and args.count <= 0:
        raise ValueError("--count must be a positive number of refresh cycles")
    try:
        asyncio.run(execute(args))
    except KeyboardInterrupt:
        pass
