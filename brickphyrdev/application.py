# SPDX-License-Identifier: Apache-2.0
"""Application operations through the standard SMP clients."""

import asyncio
import json
import math
import re
import sys
from enum import IntEnum
from pathlib import Path

import cbor2
from smp import error, message
from smpclient.requests.file_management import FileClose

from .smp import Failure, checked, connection

GROUP = 65


class Format(IntEnum):
    NATIVE = 0
    PYTHON = 1
    MPY = 2
    FILES = 3


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
    control_stack: int = 0
    control_unused: int = -1
    dropped: int
    queued: int
    received: int
    total: int
    ready: bool
    limit: int
    # Older application servers support only native ELF.
    format: Format = Format.NATIVE
    formats: int = 1
    code: int = 0
    code_used: int = 0
    repl: bool = False


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


class FormattedUpload(Upload):
    format: int


class Done(message.WriteResponse):
    # Empty acknowledgement shared by commands; checked() verifies the header.
    _GROUP_ID = GROUP
    _COMMAND_ID = 0


class Start(_Group, message.WriteRequest):
    _COMMAND_ID = 2
    _Response = Done


class StartWithStack(Start):
    stack: int


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


class OutputStream(_Group, message.WriteRequest):
    _COMMAND_ID = 4
    _Response = Done
    attached: bool


class StreamResponse(message.ReadResponse):
    _GROUP_ID = GROUP
    _COMMAND_ID = 13
    data: bytes
    state: int
    result: int
    fault: int
    cleanup: int
    dropped: int
    queued: int
    repl: bool


class Stream(_Group, message.ReadRequest):
    _COMMAND_ID = 13
    _Response = StreamResponse


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


class RunStoredWithStack(RunStored):
    stack: int


class Input(_Group, message.WriteRequest):
    _COMMAND_ID = 10
    _Response = Done
    data: bytes


class Interrupt(_Group, message.WriteRequest):
    _COMMAND_ID = 11
    _Response = Done


class Repl(_Group, message.WriteRequest):
    _COMMAND_ID = 12
    _Response = Done


class MemoryResponse(message.ReadResponse):
    _GROUP_ID = GROUP
    _COMMAND_ID = 18
    heap_total: int
    heap_used: int
    heap_free: int
    heap_peak: int
    source_ram: int
    source_flash: int
    staging: int
    python_arena: int
    primary_native: int
    control_native: int
    code: int
    code_used: int
    records: int
    gc_used: int
    gc_free: int
    gc_age_ms: int
    stack_min: int
    stack_max: int
    stack_default: int
    active: bool


class Memory(_Group, message.ReadRequest):
    _COMMAND_ID = 18
    _Response = MemoryResponse


class StackResponse(message.ReadResponse):
    _GROUP_ID = GROUP
    _COMMAND_ID = 19
    name: str = ""
    requested: int = -1
    user: int = 0
    unused: int = -1
    privileged: int = 0
    priv_unused: int = -1
    kind: str = "kernel"
    next: int = -1
    present: bool


class Stack(_Group, message.ReadRequest):
    _COMMAND_ID = 19
    _Response = StackResponse
    index: int


_RESPONSE_FIELDS = {"header", "version", "sequence", "smp_data"}


async def memory_snapshot(client):
    overview = await checked(client, Memory())
    stacks = []
    index = 0
    while True:
        entry = await checked(client, Stack(index=index))
        if not entry.present:
            break
        stacks.append(entry.model_dump(exclude=_RESPONSE_FIELDS | {"next", "present"}))
        if entry.next < 0:
            break
        if entry.next <= index:
            raise RuntimeError("Stack listing did not advance")
        index = entry.next
    return {"overview": overview.model_dump(exclude=_RESPONSE_FIELDS), "stacks": stacks}


async def check_stack(client, size):
    if size < 0:
        raise ValueError("Stack size must be nonnegative")
    if size:
        bounds = await checked(client, Memory())
        if not bounds.stack_min <= size <= bounds.stack_max:
            raise ValueError(f"Stack size must be {bounds.stack_min} through {bounds.stack_max} bytes")


def installed_name(name, *, allow_slot=False):
    if allow_slot and re.fullmatch(r"@(?:0|[1-9][0-9]{0,2})", name) and int(name[1:]) < 128:
        return name
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


def read_image(path, format=None):
    path = Path(path)
    if path.is_symlink():
        raise ValueError("Application inputs must not be symbolic links")
    if path.is_dir():
        if format not in (None, Format.FILES):
            raise ValueError("An application directory requires the files format")
        contents = {}
        for child in sorted(path.rglob("*")):
            if child.is_symlink():
                raise ValueError(f"Application contains a symbolic link: {child}")
            if child.is_file():
                contents[child.relative_to(path).as_posix()] = child.read_bytes()
        if ("main.py" in contents) == ("main.mpy" in contents):
            raise ValueError("Application directory needs exactly one main.py or main.mpy")
        return cbor2.dumps(contents, canonical=True), Format.FILES
    if format is None:
        format = {".py": Format.PYTHON, ".mpy": Format.MPY, ".cbor": Format.FILES}.get(
            path.suffix.lower(), Format.NATIVE
        )
    return path.read_bytes(), format


async def install(client, image, name, format=Format.NATIVE):
    installed_name(name)
    format = Format(format)
    if format in (Format.PYTHON, Format.MPY):
        image = cbor2.dumps({"main.py" if format == Format.PYTHON else "main.mpy": image})
        format = Format.FILES
    if format == Format.NATIVE and not image.startswith(b"\x7fELF"):
        raise ValueError("Native installation requires an ELF image")
    status = await checked(client, Status())
    if status.api != 1:
        raise RuntimeError(f"Unsupported application API {status.api}")
    if not image or len(image) > status.limit:
        raise ValueError(f"Image must contain 1 through {status.limit} bytes")
    if not status.formats & (1 << format):
        raise ValueError(f"Hub does not support {format.name.lower()} applications")
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


async def upload(client, transport, image, format=Format.NATIVE):
    format = Format(format)
    status = await checked(client, Status())
    if status.api != 1:
        raise RuntimeError(f"Unsupported application API {status.api}")
    if not image or len(image) > status.limit:
        raise ValueError(f"Image must contain 1 through {status.limit} bytes")
    if not status.formats & (1 << format):
        raise ValueError(f"Hub does not support {format.name.lower()} applications")
    request_type = Upload if format == Format.NATIVE else FormattedUpload
    arguments = {} if format == Format.NATIVE else {"format": format}
    offset = 0
    while offset < len(image):
        # CBOR overhead depends on offset/length encodings. Size the actual
        # request against the transport's negotiated decoded-packet capacity.
        count = min(len(image) - offset, transport.max_unencoded_size)
        request = request_type(
            off=offset, total=len(image), data=image[offset : offset + count], **arguments
        )
        excess = len(request.BYTES) - transport.max_unencoded_size
        if excess > 0:
            count -= excess
            if count <= 0:
                raise ValueError("SMP capacity is too small for an application fragment")
            request = request_type(
                off=offset, total=len(image), data=image[offset : offset + count], **arguments
            )
        reply = await checked(client, request)
        if reply.off != offset + count:
            raise RuntimeError(f"Upload offset {reply.off}, expected {offset + count}")
        offset = reply.off


async def launch(client, transport, args):
    stack = getattr(args, "stack", 0)
    if args.command in ("run", "start"):
        await check_stack(client, stack)
    if args.command in ("run", "upload"):
        image, format = read_image(
            args.image, Format[args.format.upper()] if args.format else None
        )
        if stack and format == Format.NATIVE:
            raise ValueError("Native ELF stack size is declared by its ABI")
        await upload(client, transport, image, format)
    if args.command == "start" and args.name:
        name = installed_name(args.name, allow_slot=True)
        request = RunStoredWithStack(name=name, stack=stack) if stack else RunStored(name=name)
        await checked(client, request)
    elif args.command in ("run", "start"):
        await checked(client, StartWithStack(stack=stack) if stack else Start())


async def execute(args):
    if args.command == "message-send":
        data = bytes.fromhex(args.hex) if args.hex is not None else Path(args.file).read_bytes()
    if args.command == "memory":
        interval = getattr(args, "interval", None)
        if interval is not None and (not math.isfinite(interval) or interval <= 0):
            raise ValueError("Memory interval must be a positive finite number of seconds")
    async with connection(ble=args.ble, port=args.port, serial_number=args.serial) as (
        client,
        transport,
    ):
        if args.command in ("message-types", "message-send", "message-status"):
            from . import host_messages

            if args.command == "message-types":
                entries = await host_messages.describe(client)
                value = [entry.model_dump(exclude=_RESPONSE_FIELDS | {"present"}) |
                         {"version": entry.message_version} for entry in entries]
            elif args.command == "message-send":
                reply = await host_messages.send(client, args.type, data, args.request)
                value = reply.model_dump(exclude=_RESPONSE_FIELDS)
            else:
                reply = await host_messages.status(client, args.type, args.request)
                value = reply.model_dump(exclude=_RESPONSE_FIELDS)
            print(json.dumps(value))
            return
        if args.command == "memory":
            while True:
                print(json.dumps(await memory_snapshot(client)))
                if interval is None:
                    return
                await asyncio.sleep(interval)
        if args.command == "record-clear":
            from .recording import Clear

            await checked(client, Clear())
            return
        if args.command == "list":
            async for entry in files(client):
                print(f"{entry.name}\t{entry.size}")
            return
        if args.command == "install":
            image, format = read_image(args.image)
            await install(client, image, args.name or Path(args.image).stem, format)
        elif args.command == "remove":
            await checked(client, Remove(name=installed_name(args.name, allow_slot=True)))
        if args.command in ("run", "upload", "start"):
            await launch(client, transport, args)
        elif args.command == "stop":
            await checked(client, Stop())
        elif args.command == "interrupt":
            await checked(client, Interrupt())
        elif args.command == "input":
            data = (args.text + ("" if args.no_newline else "\n")).encode()
            if len(data) > 128:
                raise ValueError("Input accepts at most 128 UTF-8 bytes per command")
            await checked(client, Input(data=data))
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
            value["format"] = status.format.name.lower()
            print(json.dumps(value, indent=2))


def run(args):
    if getattr(args, "follow", False) or getattr(args, "record", None):
        from . import follow

        follow.run(args)
        return
    if args.command == "record":
        from . import recording

        recording.run(args)
        return
    try:
        asyncio.run(execute(args))
    except KeyboardInterrupt:
        if args.command != "memory":
            raise
