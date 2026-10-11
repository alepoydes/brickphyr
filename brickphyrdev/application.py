# SPDX-License-Identifier: Apache-2.0
"""Native application operations through the standard SMP clients."""

import asyncio
import contextlib
import json
import sys
from pathlib import Path

from pydantic import BaseModel
from smp import error, message
from smpclient import SMPClient, success
from smpclient.generics import error_v2
from smpclient.transport.ble import SMPBLETransport
from smpclient.transport.serial import SMPSerialTransport
from usb.util import dispose_resources

from . import usb

GROUP = 65


class ErrorV1(error.ErrorV1):
    _GROUP_ID = GROUP


class Failure(BaseModel):
    group: int
    rc: int


class ErrorV2(error.ErrorV2):
    _GROUP_ID = GROUP
    err: Failure


class _Group:
    _GROUP_ID = GROUP
    _ErrorV1 = ErrorV1
    _ErrorV2 = ErrorV2


class StatusResponse(message.ReadResponse):
    _GROUP_ID = GROUP
    _COMMAND_ID = 0
    api: int
    abi: str
    state: int
    result: int
    fault: int
    cleanup: int
    image: int
    heap: int
    loaded: int
    stack: int
    unused: int
    dropped: int
    queued: int
    received: int
    total: int
    ready: bool
    limit: int


class Status(_Group, message.ReadRequest):
    _COMMAND_ID = 0
    _Response = StatusResponse


class Uploaded(message.WriteResponse):
    _GROUP_ID = GROUP
    _COMMAND_ID = 1
    off: int


class Upload(_Group, message.WriteRequest):
    _COMMAND_ID = 1
    _Response = Uploaded
    off: int
    total: int
    data: bytes


class Done(message.WriteResponse):
    # Empty acknowledgement shared by commands; checked() verifies the header.
    _GROUP_ID = GROUP
    _COMMAND_ID = 0


class Start(_Group, message.WriteRequest):
    _COMMAND_ID = 2
    _Response = Done


class Stop(_Group, message.WriteRequest):
    _COMMAND_ID = 3
    _Response = Done


class OutputResponse(message.ReadResponse):
    _GROUP_ID = GROUP
    _COMMAND_ID = 4
    data: bytes


class Output(_Group, message.ReadRequest):
    _COMMAND_ID = 4
    _Response = OutputResponse


class Discard(_Group, message.WriteRequest):
    _COMMAND_ID = 5
    _Response = Done


async def checked(client, request):
    reply = await client.request(request)
    if (reply.header.group_id, reply.header.command_id, int(reply.header.op)) != (
        request.header.group_id,
        request.header.command_id,
        int(request.header.op) + 1,
    ):
        raise RuntimeError("Mismatched SMP response header")
    if not success(reply):
        detail = (
            f"group {reply.err.group}, error {reply.err.rc}"
            if error_v2(reply)
            else f"SMP error {reply.rc}"
        )
        raise RuntimeError(f"{type(request).__name__} rejected: {detail}")
    return reply


@contextlib.asynccontextmanager
async def connection(*, ble=None, port=None, serial_number=None):
    hub = None
    client = None
    try:
        if ble:
            transport, address = SMPBLETransport(), ble
        else:
            if port is None:
                hub = usb.device(serial_number)
                usb.select(hub, "shell")
                port = usb.serial_port(hub)
            transport, address = SMPSerialTransport(exclusive=True, write_timeout=3), port
        client = SMPClient(transport, address, timeout_s=5)
        await client.connect()
        yield client, transport
    finally:
        # Preserve the operation's first error if releasing a link also fails.
        original = sys.exception()
        try:
            if client:
                if original:
                    with contextlib.suppress(Exception):
                        await client.disconnect()
                else:
                    await client.disconnect()
        finally:
            if hub is not None:
                if sys.exception() is not None:
                    with contextlib.suppress(Exception):
                        dispose_resources(hub)
                else:
                    dispose_resources(hub)


async def upload(client, transport, image):
    status = await checked(client, Status())
    if status.api != 1:
        raise RuntimeError(f"Unsupported application API {status.api}")
    if not image or len(image) > status.limit:
        raise ValueError(f"Image must contain 1 through {status.limit} bytes")
    offset = 0
    while offset < len(image):
        # CBOR overhead depends on offset/length encodings. Size the actual
        # request against the transport's negotiated decoded-packet capacity.
        count = min(len(image) - offset, transport.max_unencoded_size)
        request = Upload(off=offset, total=len(image), data=image[offset : offset + count])
        excess = len(request.BYTES) - transport.max_unencoded_size
        if excess > 0:
            count -= excess
            if count <= 0:
                raise ValueError("SMP capacity is too small for an application fragment")
            request = Upload(off=offset, total=len(image), data=image[offset : offset + count])
        reply = await checked(client, request)
        if reply.off != offset + count:
            raise RuntimeError(f"Upload offset {reply.off}, expected {offset + count}")
        offset = reply.off


async def execute(args):
    async with connection(ble=args.ble, port=args.port, serial_number=args.serial) as (
        client,
        transport,
    ):
        if args.command in ("run", "upload"):
            await upload(client, transport, Path(args.image).read_bytes())
        if args.command in ("run", "start"):
            await checked(client, Start())
        elif args.command == "stop":
            await checked(client, Stop())
        elif args.command == "discard":
            await checked(client, Discard())
        status = await checked(client, Status())
        if args.command == "output":
            remaining = status.queued
            while remaining:
                data = (await checked(client, Output())).data
                if not data:
                    break
                sys.stdout.buffer.write(data)
                remaining -= min(remaining, len(data))
            sys.stdout.buffer.flush()
        else:
            value = status.model_dump(exclude={"header", "version", "sequence", "smp_data"})
            value["state"] = ("idle", "running", "returned", "stopped", "fault")[status.state]
            print(json.dumps(value, indent=2))


def run(args):
    asyncio.run(execute(args))
