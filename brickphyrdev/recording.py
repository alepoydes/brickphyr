# SPDX-License-Identifier: Apache-2.0
"""Collect retained application cycle records over SMP."""

import asyncio
import contextlib
import uuid
from pathlib import Path

from pydantic import Field
from smp import message

from . import application, monitor, record_codec, records, telemetry
from .recording_io import write_event
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
    block: int = 0
    tag: bytes | None = None


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


class BlockResponse(RecordResponse):
    _COMMAND_ID = 22


class Block(Record):
    _COMMAND_ID = 22
    _Response = BlockResponse


class BindResponse(message.WriteResponse):
    _GROUP_ID = application.GROUP
    _COMMAND_ID = 23
    generation: int
    tag: bytes


class Bind(application._Group, message.WriteRequest):
    _COMMAND_ID = 23
    _Response = BindResponse
    generation: int
    tag: bytes


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


async def collect(client, log, session, counts, *, after=None, wait=False, resume=None):
    if resume is not None and after is not None:
        raise ValueError("Cannot trigger a recording while resuming its download")
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
    block = getattr(info, "block", 0)
    if not 0 <= block <= 1024:
        raise RuntimeError("Invalid recording block capacity")
    step_size = 32 if block and info.control else 0
    stride = step_size + info.size

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

    tag = getattr(info, "tag", None)
    if tag is not None and len(tag) != 16:
        raise RuntimeError("Invalid recording tag")
    if resume is not None:
        saved = resume.session["recording"]
        if not resume.tag or tag != resume.tag or generation != saved["generation"]:
            raise RuntimeError("Recording identity changed; cannot resume this download")
        if _identity(info) != (
            saved["size"], saved["capacity"], saved["schema"], saved.get("config_size", 0),
            saved.get("control", False), saved.get("parameter_size", 0),
        ) or bytes(config) != resume.config:
            raise RuntimeError("Recording metadata changed; cannot resume this download")
        _advance((saved["first"], saved["next"], saved.get("trigger"),
                  saved.get("after", 0), saved["reason"]),
                 (info.first, info.next, trigger, trigger_after, reason))
        if resume.next_index > info.next:
            raise RuntimeError("Recording index range moved backwards")
        if resume.needs_separator:
            log.write("\n")
        write_event(log, {"type": "resume", "session": session, "from": resume.next_index,
                          "resumed_utc": monitor.utc_now()})
    else:
        if tag is not None:
            bound = await checked(client, Bind(generation=generation, tag=uuid.uuid4().bytes))
            if bound.generation != generation or len(bound.tag) != 16 or not any(bound.tag):
                raise RuntimeError("Invalid bound recording identity")
            tag = bound.tag
        identity = await telemetry.info(client)
        status = await checked(client, application.Status())
        write_event(
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
                    "config": bytes(config),
                    "config_size": info.config_size,
                    "trigger": trigger,
                    "after": trigger_after,
                    "reason": reason,
                    "control": info.control,
                    "parameter_size": info.parameter_size,
                    "tag": tag,
                },
            },
        )

    first, next, active = info.first, info.next, info.active
    index = resume.next_index if resume else 0
    partial = bytearray()
    partial_step = None
    previous = resume.last_control if resume else None
    last_sequence = previous["sequence"] if previous else None
    last_revision = previous["parameter_revision"] if previous else None
    cached_revision = resume.parameter_revision if resume else None
    while True:
        if index < first:
            write_event(log, {"type": "gap", "from": index, "to": first})
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
            write_event(log, event)
            return

        request = Block if block else Record
        reply = await checked(client, request(generation=generation, index=index, off=len(partial)))
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
        if info.control and not block:
            step = record_codec.control_step(reply.step, info.parameter_size)
            if partial_step is None:
                partial_step = reply.step
            elif reply.step != partial_step:
                raise RuntimeError("Control step changed between record fragments")
        elif reply.step:
            raise RuntimeError("Unexpected control step on ordinary recording")
        limit = block or (224 if info.control else 256)
        available = (next - index) * stride if block else info.size
        if not reply.data or len(reply.data) > limit or len(reply.data) > available - len(partial):
            raise RuntimeError("Invalid recording fragment")
        partial.extend(reply.data)
        while len(partial) >= stride:
            if info.control:
                if block:
                    step = record_codec.control_step(partial[:step_size], info.parameter_size)
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
                        break  # Discard the block together with overwritten parameters.
                    if index >= next or params.revision != revision or (
                        len(params.data) != info.parameter_size
                    ):
                        raise RuntimeError("Invalid retained control parameters")
                    write_event(log, {
                        "type": "parameters", "revision": revision, "data": params.data,
                    })
                    cached_revision = revision
            data = partial[step_size:stride]
            record = {"type": "record", "index": index, "data": bytes(data)}
            if info.control:
                record["control"] = step
            values = record_codec.decode(info.record_schema, data)
            if values is not None:
                record["values"] = values
            write_event(log, record)
            counts["records"] += 1
            if info.control:
                last_sequence, last_revision = sequence, revision
            index += 1
            del partial[:stride]
            partial_step = None


@contextlib.contextmanager
def session(log, *, resume=None):
    session_id = resume.session["session"] if resume else str(uuid.uuid4())
    failure = None
    counts = {"records": resume.count if resume else 0, "gaps": resume.gaps if resume else 0}
    offset = log.tell() if resume else None
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
        if resume and log.tell() == offset:
            pass  # Failed identity/connection checks leave the original file intact.
        elif failure:
            with contextlib.suppress(Exception):
                write_event(log, end)
        else:
            write_event(log, end)


async def _record(args, log, *, resume=None):
    with session(log, resume=resume) as (session_id, counts):
        async with connection(ble=args.ble, port=args.port, serial_number=args.serial) as (
            client,
            _,
        ):
            await collect(client, log, session_id, counts, after=getattr(args, "after", None),
                          resume=resume)


@contextlib.contextmanager
def open_log(path, format=None, *, resume=False):
    selected = format or ("cbor" if path and Path(path).suffix.lower() == ".cbor" else "jsonl")
    if selected not in ("cbor", "jsonl"):
        raise ValueError("Recording format must be cbor or jsonl")
    with monitor.recording(path, binary=selected == "cbor", resume=resume) as log:
        yield log


def run(args):
    resume = getattr(args, "resume", False)
    with open_log(args.file, getattr(args, "format", None), resume=resume) as log:
        saved = None
        if resume:
            saved = records.Recording(log)
            for _ in saved.frames(allow_gaps=True, allow_incomplete=True):
                pass
            if not saved.tag:
                raise ValueError("Recording has no capture identity; cannot resume")
            if saved.complete or saved.outcome is not None:
                raise ValueError("Recording payload is already complete")
            log.seek(0, 2)
        asyncio.run(_record(args, log, resume=saved))
