# SPDX-License-Identifier: Apache-2.0
"""Collect retained application cycle records over SMP."""

import asyncio
import contextlib
import uuid

from pydantic import Field
from smp import message

from . import application, monitor, record_codec, telemetry
from .smp import checked, connection


class InfoResponse(message.ReadResponse):
    _GROUP_ID = application.GROUP
    _COMMAND_ID = 14
    generation: int
    size: int
    capacity: int
    first: int
    next: int
    active: bool
    record_schema: str = Field(alias="schema")
    config_size: int = 0
    trigger: int | None = None
    after: int = 0
    reason: int | None = None
    control: bool = False
    parameter_size: int = 0


class Info(application._Group, message.ReadRequest):
    _COMMAND_ID = 14
    _Response = InfoResponse


class Clear(application._Group, message.WriteRequest):
    _COMMAND_ID = 14
    _Response = application.Done


class RecordResponse(message.ReadResponse):
    _GROUP_ID = application.GROUP
    _COMMAND_ID = 15
    generation: int
    first: int
    next: int
    active: bool
    data: bytes
    trigger: int | None = None
    after: int = 0
    reason: int | None = None
    step: bytes = b""


class Record(application._Group, message.ReadRequest):
    _COMMAND_ID = 15
    _Response = RecordResponse
    generation: int
    index: int
    off: int


class ConfigResponse(message.ReadResponse):
    _GROUP_ID = application.GROUP
    _COMMAND_ID = 16
    generation: int
    total: int
    data: bytes


class Config(application._Group, message.ReadRequest):
    _COMMAND_ID = 16
    _Response = ConfigResponse
    generation: int
    off: int


class Trigger(application._Group, message.WriteRequest):
    _COMMAND_ID = 16
    _Response = application.Done
    generation: int
    after: int


class ParametersResponse(message.ReadResponse):
    _GROUP_ID = application.GROUP
    _COMMAND_ID = 17
    generation: int
    first: int
    next: int
    revision: int
    data: bytes


class Parameters(application._Group, message.ReadRequest):
    _COMMAND_ID = 17
    _Response = ParametersResponse
    generation: int
    index: int


def _range(first, next):
    if not 0 <= first <= next < 1 << 64:
        raise RuntimeError("Invalid recording index range")


def _identity(info):
    return (info.size, info.capacity, info.record_schema, info.config_size,
            info.control, info.parameter_size)


def _metadata(reply, capacity, *, config_size=0):
    _range(reply.first, reply.next)
    trigger, after, reason = reply.trigger, reply.after, reply.reason
    if reason is None:
        if trigger is not None or after != 0 or config_size != 0:
            raise RuntimeError("Incomplete recording capture metadata")
        reason = 0 if reply.active else 1
    if reason not in (0, 1, 2) or reply.active != (reason == 0):
        raise RuntimeError("Invalid recording end reason")
    if trigger is None:
        if after != 0 or reason == 2:
            raise RuntimeError("Invalid recording trigger metadata")
    elif not (0 < trigger < 1 << 64 and trigger <= reply.next and
              0 <= after < capacity and trigger + after < 1 << 64):
        raise RuntimeError("Invalid recording trigger metadata")
    if reason == 2 and reply.next != trigger + after:
        raise RuntimeError("Invalid triggered recording boundary")
    if trigger is not None and reason != 2 and reply.next >= trigger + after:
        raise RuntimeError("Invalid open recording trigger boundary")
    return trigger, after, reason


def _advance(old, new):
    old_first, old_next, old_trigger, old_after, old_reason = old
    first, next_index, trigger, after, reason = new
    if first < old_first or next_index < old_next:
        raise RuntimeError("Recording index range moved backwards")
    if (old_trigger is not None and (trigger, after) != (old_trigger, old_after)) or (
        old_reason != 0 and reason != old_reason
    ):
        raise RuntimeError("Recording trigger state changed")


async def collect(client, log, session, counts, *, after=None, wait=False):
    info = await checked(client, Info())
    while wait and info.size == 0 and info.active:
        await asyncio.sleep(0.05)
        info = await checked(client, Info())
    if info.size == 0:
        raise RuntimeError("Recording not initialized")
    if not 0 < info.generation < 1 << 32:
        raise RuntimeError("Invalid recording metadata")
    if not 0 < info.size <= 1024 or not 0 < info.capacity < 1 << 32 or (
        not 0 <= info.config_size <= 1024 or
        info.size * info.capacity + info.config_size > 32768
    ):
        raise RuntimeError("Invalid recording size or capacity")
    if not 0 <= info.parameter_size <= 256 or not info.control and info.parameter_size:
        raise RuntimeError("Invalid recording control parameter size")
    trigger, trigger_after, reason = _metadata(
        info, info.capacity, config_size=info.config_size
    )
    if not info.record_schema or len(info.record_schema) > 47 or any(
        not 32 <= ord(char) <= 126 for char in info.record_schema
    ):
        raise RuntimeError("Invalid recording schema")
    record_codec.check_size(info.record_schema, info.size)

    generation = info.generation
    if after is not None:
        if not 0 <= after < info.capacity:
            raise ValueError("--after must be between zero and recording capacity minus one")
        if info.first == info.next:
            raise RuntimeError("Cannot trigger an empty recording")
        await checked(client, Trigger(generation=generation, after=after))
        triggered_info = await checked(client, Info())
        if triggered_info.generation != generation:
            raise RuntimeError(
                f"Recording generation changed from {generation} to {triggered_info.generation}"
            )
        if _identity(triggered_info) != _identity(info):
            raise RuntimeError("Recording metadata changed within generation")
        state = _metadata(
            triggered_info, info.capacity, config_size=triggered_info.config_size
        )
        _advance((info.first, info.next, trigger, trigger_after, reason),
                 (triggered_info.first, triggered_info.next, *state))
        if state[0] is None or state[1] != after:
            raise RuntimeError("Recording trigger was not retained")
        info = triggered_info
        trigger, trigger_after, reason = state

    config = bytearray()
    while len(config) < info.config_size:
        fragment = await checked(client, Config(generation=generation, off=len(config)))
        if fragment.generation != generation:
            raise RuntimeError(
                f"Recording generation changed from {generation} to {fragment.generation}"
            )
        if fragment.total != info.config_size or not 0 < len(fragment.data) <= min(
            256, info.config_size - len(config)
        ):
            raise RuntimeError("Invalid recording config fragment")
        config.extend(fragment.data)
    if info.config_size:
        current = await checked(client, Info())
        if current.generation != generation:
            raise RuntimeError(
                f"Recording generation changed from {generation} to {current.generation}"
            )
        if _identity(current) != _identity(info):
            raise RuntimeError("Recording metadata changed within generation")
        state = _metadata(current, info.capacity, config_size=current.config_size)
        _advance((info.first, info.next, trigger, trigger_after, reason),
                 (current.first, current.next, *state))
        info = current
        trigger, trigger_after, reason = state

    identity = await telemetry.info(client)
    status = await checked(client, application.Status())
    monitor.write_log(
        log,
        {
            "type": "session",
            "schema": "brickphyr.records/1",
            "session": session,
            "started_utc": monitor.utc_now(),
            "firmware": identity.firmware,
            "build": identity.build,
            "board": identity.board,
            "application": status.model_dump(
                exclude={"header", "version", "sequence", "smp_data"}
            ),
            "recording": {
                "generation": info.generation,
                "size": info.size,
                "capacity": info.capacity,
                "first": info.first,
                "next": info.next,
                "active": info.active,
                "schema": info.record_schema,
                "config": config.hex(),
                "config_size": info.config_size,
                "trigger": trigger,
                "after": trigger_after,
                "reason": reason,
                "control": info.control,
                "parameter_size": info.parameter_size,
            },
        },
    )

    first, next, active = info.first, info.next, info.active
    index = 0
    partial = bytearray()
    partial_step = None
    last_sequence = last_revision = cached_revision = None
    while True:
        if index < first:
            monitor.write_log(log, {"type": "gap", "from": index, "to": first})
            counts["gaps"] += 1
            index = first
            partial.clear()
            partial_step = None
        if not active and index >= next:
            status = await checked(client, application.Status())
            terminal_info = await checked(client, Info())
            if terminal_info.generation != generation:
                raise RuntimeError(
                    f"Recording generation changed from {generation} to {terminal_info.generation}"
                )
            if _identity(terminal_info) != _identity(info):
                raise RuntimeError("Recording metadata changed within generation")
            state = _metadata(
                terminal_info, info.capacity, config_size=terminal_info.config_size
            )
            _advance((first, next, trigger, trigger_after, reason),
                     (terminal_info.first, terminal_info.next, *state))
            first, next, active = terminal_info.first, terminal_info.next, terminal_info.active
            trigger, trigger_after, reason = state
            if active or next > index:
                continue
            event = {
                "type": "recording-end" if reason == 2 else "application-end",
                "state": status.state,
                "result": status.result,
                "fault": status.fault,
                "cleanup": status.cleanup,
            }
            if trigger is not None:
                event["capture"] = {
                    "trigger": trigger, "after": trigger_after, "next": next,
                    "complete": reason == 2,
                }
            if reason == 2:
                event["reason"] = "trigger"
            monitor.write_log(log, event)
            return

        reply = await checked(client, Record(generation=generation, index=index, off=len(partial)))
        if reply.generation != generation:
            raise RuntimeError(
                f"Recording generation changed from {generation} to {reply.generation}"
            )
        state = _metadata(reply, info.capacity)
        _advance((first, next, trigger, trigger_after, reason),
                 (reply.first, reply.next, *state))
        first, next, active = reply.first, reply.next, reply.active
        trigger, trigger_after, reason = state
        if index < first:
            continue  # Discard the partial record before the next request.
        if index >= next:
            if reply.data or reply.step:
                raise RuntimeError("Recording data outside retained range")
            if active:
                await asyncio.sleep(0.02)
            continue
        if info.control:
            step = record_codec.control_step(reply.step, info.parameter_size)
            if partial_step is None:
                partial_step = reply.step
            elif reply.step != partial_step:
                raise RuntimeError("Control step changed between record fragments")
        elif reply.step:
            raise RuntimeError("Unexpected control step on ordinary recording")
        limit = 224 if info.control else 256
        if not reply.data or len(reply.data) > limit or len(reply.data) > info.size - len(partial):
            raise RuntimeError("Invalid recording fragment")
        partial.extend(reply.data)
        if len(partial) == info.size:
            if info.control:
                sequence, revision = step["sequence"], step["parameter_revision"]
                if (last_sequence is not None and sequence < last_sequence or
                    last_revision is not None and revision < last_revision):
                    raise RuntimeError("Control step sequence or revision moved backwards")
                if revision != cached_revision:
                    params = await checked(client, Parameters(generation=generation, index=index))
                    if params.generation != generation:
                        raise RuntimeError(
                            f"Recording generation changed from {generation} to {params.generation}"
                        )
                    _range(params.first, params.next)
                    if params.first < first or params.next < next:
                        raise RuntimeError("Recording index range moved backwards")
                    first, next = params.first, params.next
                    if index < first:
                        continue  # The record and its parameter snapshot were overwritten.
                    if index >= next or params.revision != revision or (
                        len(params.data) != info.parameter_size
                    ):
                        raise RuntimeError("Invalid retained control parameters")
                    monitor.write_log(log, {
                        "type": "parameters", "revision": revision, "data": params.data.hex(),
                    })
                    cached_revision = revision
            record = {"type": "record", "index": index, "data": partial.hex()}
            if info.control:
                record["control"] = step
            values = record_codec.decode(info.record_schema, partial)
            if values is not None:
                record["values"] = values
            monitor.write_log(log, record)
            counts["records"] += 1
            if info.control:
                last_sequence, last_revision = sequence, revision
            index += 1
            partial.clear()
            partial_step = None


@contextlib.contextmanager
def session(log):
    session_id = str(uuid.uuid4())
    failure = None
    counts = {"records": 0, "gaps": 0}
    try:
        yield session_id, counts
    except BaseException as error:
        failure = error
        raise
    finally:
        end = {
            "type": "end",
            "session": session_id,
            "ended_utc": monitor.utc_now(),
            "reason": "cancelled"
            if isinstance(failure, (asyncio.CancelledError, KeyboardInterrupt))
            else "error"
            if failure
            else "completed",
            **counts,
        }
        if failure and str(failure):
            end["error"] = str(failure)
        if failure:
            with contextlib.suppress(Exception):
                monitor.write_log(log, end)
        else:
            monitor.write_log(log, end)


async def _record(args, log):
    with session(log) as (session_id, counts):
        async with connection(ble=args.ble, port=args.port, serial_number=args.serial) as (
            client,
            _,
        ):
            await collect(client, log, session_id, counts, after=getattr(args, "after", None))


def run(args):
    with monitor.recording(args.file) as log:
        asyncio.run(_record(args, log))
