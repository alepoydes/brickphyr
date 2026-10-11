# SPDX-License-Identifier: Apache-2.0
"""Native application operations through the standard SMP clients."""

import asyncio
import json
import sys
from pathlib import Path

from smp import error, message
from smpclient.requests.file_management import FileClose

from .smp import Failure, checked, connection

GROUP = 65


class ErrorV1(error.ErrorV1):
    _GROUP_ID = GROUP


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


class FileInfo(message.ReadResponse):
    _GROUP_ID = GROUP
    _COMMAND_ID = 6
    name: str
    size: int
    available: int


class ListFiles(_Group, message.ReadRequest):
    _COMMAND_ID = 6
    _Response = FileInfo
    name: str


class Publish(_Group, message.WriteRequest):
    _COMMAND_ID = 7
    _Response = Done
    name: str
    size: int


class Remove(_Group, message.WriteRequest):
    _COMMAND_ID = 8
    _Response = Done
    name: str


class RunStored(_Group, message.WriteRequest):
    _COMMAND_ID = 9
    _Response = Done
    name: str


def installed_name(name):
    if (
        not 1 <= len(name) <= 32
        or name.startswith(".")
        or any(
            c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
            for c in name
        )
    ):
        raise ValueError(
            "Application names use 1–32 ASCII letters, digits, '.', '_' or '-'; no leading '.'"
        )
    return name


async def install(client, image, name):
    installed_name(name)
    status = await checked(client, Status())
    if status.api != 1:
        raise RuntimeError(f"Unsupported application API {status.api}")
    if not image or len(image) > status.limit:
        raise ValueError(f"Image must contain 1 through {status.limit} bytes")
    # Cancel a previous partial standard transfer before reusing its staging path.
    await checked(client, FileClose())
    previous = 0
    async for offset in client.upload_file(image, "/apps/.incoming"):
        if not previous < offset <= len(image):
            raise RuntimeError(f"Invalid file upload offset {offset}")
        previous = offset
    await checked(client, Publish(name=name, size=len(image)))


async def files(client):
    after = ""
    while True:
        entry = await checked(client, ListFiles(name=after))
        if not entry.name:
            return
        if entry.name <= after:
            raise RuntimeError("File listing did not advance")
        yield entry
        after = entry.name


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
        if args.command == "list":
            async for entry in files(client):
                print(f"{entry.name}\t{entry.size}")
            return
        if args.command == "install":
            await install(client, Path(args.image).read_bytes(), args.name or Path(args.image).stem)
        elif args.command == "remove":
            await checked(client, Remove(name=installed_name(args.name)))
        if args.command in ("run", "upload"):
            await upload(client, transport, Path(args.image).read_bytes())
        if args.command == "start" and args.name:
            await checked(client, RunStored(name=installed_name(args.name)))
        elif args.command in ("run", "start"):
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
