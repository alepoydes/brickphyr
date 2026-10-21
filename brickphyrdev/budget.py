# SPDX-License-Identifier: Apache-2.0
"""Read-only application budget preflight through firmware accounting."""

import asyncio
import json

from smp import message

from . import application
from .smp import checked, connection


class ImageBudgetResponse(message.ReadResponse):
    _GROUP_ID = application.GROUP
    _COMMAND_ID = 20
    format: application.Format
    image: int
    source_ram: int
    source_flash: int
    transfer: int
    stack: int
    user_stack: int
    priv_stack: int
    arena: int
    code_limit: int
    native: int
    alignment: int
    supervisor: int


class ImageBudget(application._Group, message.ReadRequest):
    _COMMAND_ID = 20
    _Response = ImageBudgetResponse
    name: str = ""
    resource: str = ""
    control: bool = False
    stack: int = 0
    parameters: int = 0


class RecordBudgetResponse(message.ReadResponse):
    _GROUP_ID = application.GROUP
    _COMMAND_ID = 21
    bytes: int


class RecordBudget(application._Group, message.ReadRequest):
    _COMMAND_ID = 21
    _Response = RecordBudgetResponse
    size: int
    capacity: int
    config: int = 0
    control: bool = False
    parameters: int = 0


_RESPONSE_FIELDS = {"header", "version", "sequence", "smp_data"}
_RESIDENT_FIELDS = ("source_ram", "user_stack", "priv_stack", "arena", "native", "supervisor")
_MINIMUM_FIELDS = ("source_ram", "user_stack", "priv_stack", "arena")


def _image_dict(reply):
    value = reply.model_dump(exclude=_RESPONSE_FIELDS)
    value["format"] = reply.format.name.lower()
    return value


def _sum_fields(images, fields):
    return sum(image[field] for image in images for field in fields)


def _validate(args):
    for option in ("stack", "parameters", "record_size", "record_count", "record_config"):
        value = getattr(args, option)
        if value is not None and value < 0:
            raise ValueError(f"--{option.replace('_', '-')} must be nonnegative")
    if args.dynamic is not None and args.dynamic < 0:
        raise ValueError("--dynamic must be nonnegative")
    if (args.record_size is None) != (args.record_count is None):
        raise ValueError("--record-size and --record-count must be provided together")
    if args.record_size is not None and (args.record_size == 0 or args.record_count == 0):
        raise ValueError("--record-size and --record-count must be positive")
    if args.record_config and args.record_size is None:
        raise ValueError("--record-config requires --record-size and --record-count")
    if args.parameters and not (args.controller or args.resource):
        raise ValueError("--parameters requires --controller or --resource")
    if args.name:
        application.installed_name(args.name, allow_slot=True)
    if args.controller:
        application.installed_name(args.controller, allow_slot=True)


async def inspect(client, args):
    """Return known reservations and separate unknown peak/fit constraints."""
    _validate(args)
    memory = await application.memory_snapshot(client)
    try:
        primary = _image_dict(await checked(client, ImageBudget(name=args.name or "", stack=args.stack)))
    except Exception as error:
        raise RuntimeError(f"Primary budget read failed: {error}") from error
    if args.stack and primary["format"] == "native":
        raise ValueError("Native stack size comes from its ABI and cannot be overridden")
    if args.native_code and primary["format"] not in ("python", "mpy", "files"):
        raise ValueError("--native-code applies only to Python applications")

    controller = None
    if args.controller or args.resource:
        try:
            controller = _image_dict(await checked(client, ImageBudget(
                name=args.controller or args.name or "", resource=args.resource or "",
                control=True, parameters=args.parameters,
            )))
        except Exception as error:
            raise RuntimeError(f"Controller budget read failed: {error}") from error
    images = [primary] + ([controller] if controller is not None else [])
    recorder = {"status": "unknown"}
    if args.record_size is not None:
        response = await checked(client, RecordBudget(
            size=args.record_size, capacity=args.record_count, config=args.record_config,
            control=controller is not None, parameters=args.parameters,
        ))
        recorder = {"status": "declared", "bytes": response.bytes,
                    "size": args.record_size, "capacity": args.record_count,
                    "config": args.record_config}
    dynamic = ({"status": "unknown"} if args.dynamic is None else
               {"status": "declared", "bytes": args.dynamic})

    accounted = _sum_fields(images, _RESIDENT_FIELDS)
    if args.native_code:
        accounted += primary["code_limit"]
    if recorder["status"] == "declared":
        accounted += recorder["bytes"]
    if dynamic["status"] == "declared":
        accounted += dynamic["bytes"]

    overview = memory["overview"]
    minimum = _sum_fields(images, _MINIMUM_FIELDS)
    transfer = sum(image["transfer"] for image in images)
    # Preparation keeps the old application until the new primary is ready.
    # A ready RAM source is already in heap_used, and a Python arena can be
    # reused. Controller, recorder and native-code allocations follow release.
    before_release = (overview["heap_used"] + primary["user_stack"] +
                      primary["priv_stack"] + primary["supervisor"])
    if primary["format"] == "native":
        before_release += primary["native"]
    else:
        if not overview["python_arena"]:
            before_release += primary["arena"]
    unknown = ["allocator and kernel-object overhead", "alignment and fragmentation",
               "transient loader allocations"]
    if args.dynamic is None:
        unknown.append("undeclared runtime allocation")
    if args.record_size is None:
        unknown.append("undeclared recorder allocation")
    if primary["format"] in ("python", "mpy", "files") and not args.native_code:
        unknown.append("undeclared Python native-code allocation")

    return {
        "primary": primary,
        "controller": controller,
        "recorder": recorder,
        "dynamic": dynamic,
        "native_code_declared": args.native_code,
        "resident_accounted": accounted,
        "current": {"heap_total": overview["heap_total"], "heap_used": overview["heap_used"],
                    "heap_free": overview["heap_free"], "heap_peak": overview["heap_peak"],
                    "stacks": memory["stacks"]},
        "staged_transfer": overview["staging"],
        "phases": {
            "load": {"accounted": accounted + transfer, "peak": "unknown"},
            "replacement": {"before_release_accounted": before_release, "peak": "unknown"},
        },
        "admission": {"status": "insufficient_minimum" if minimum > overview["heap_total"]
                      else "fit_unknown", "minimum_required": minimum,
                      "heap_total": overview["heap_total"]},
        "unknown": unknown,
    }


async def execute(args):
    _validate(args)
    async with connection(ble=args.ble, port=args.port, serial_number=args.serial) as (client, _):
        print(json.dumps(await inspect(client, args), indent=2))


def run(args):
    asyncio.run(execute(args))
