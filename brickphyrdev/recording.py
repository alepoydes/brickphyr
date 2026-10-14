# SPDX-License-Identifier: Apache-2.0
"""Collect retained application cycle records over SMP."""

import asyncio
import contextlib
import struct
import uuid

from pydantic import Field
from smp import message

from . import application, monitor, telemetry
from .smp import checked, connection

_CONTROL_SCHEMA = "brickphyr.control-input/1"
_CONTROL_RECORD = struct.Struct("<8q10i6I2q4i4I")
_GROUP_SCHEMA = "brickphyr.control-group/1"
_GROUP_TAIL = struct.Struct("<3q4I3i2Ii3q3I2iI")
_GROUP_PORT = struct.Struct("<2I2iq4I2i")


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


class Record(application._Group, message.ReadRequest):
    _COMMAND_ID = 15
    _Response = RecordResponse
    generation: int
    index: int
    off: int


def _range(first, next):
    if not 0 <= first <= next < 1 << 64:
        raise RuntimeError("Invalid recording index range")


def _control_values(data):
    if len(data) != _CONTROL_RECORD.size:
        raise RuntimeError("Invalid brickphyr.control-input/1 record length")
    (
        capture_start, capture_end, imu_sample, imu_read,
        port_a_sample, port_b_sample, angle_a, angle_b,
        accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z,
        imu_error, port_a_error, port_b_error, battery_error,
        imu_epoch, imu_sequence, port_a_generation, port_b_generation,
        battery_mv, capture_resolution,
        applied_a, applied_b,
        duty_a, duty_b, output_a_error, output_b_error,
        completed_a, completed_b, output_a_generation, output_b_generation,
    ) = _CONTROL_RECORD.unpack(data)
    return {
        "capture_start_us": capture_start,
        "capture_end_us": capture_end,
        "imu_sample_us": imu_sample,
        "imu_read_us": imu_read,
        "port_sample_ms": [port_a_sample, port_b_sample],
        "angle_mdeg": [angle_a, angle_b],
        "acceleration": [accel_x, accel_y, accel_z],
        "angular_velocity": [gyro_x, gyro_y, gyro_z],
        "errors": [imu_error, port_a_error, port_b_error, battery_error],
        "imu_epoch": imu_epoch,
        "imu_sequence": imu_sequence,
        "port_generation": [port_a_generation, port_b_generation],
        "battery_mv": battery_mv,
        "capture_resolution_us": capture_resolution,
        "applied_ms": [applied_a, applied_b],
        "duty_permille": [duty_a, duty_b],
        "output_errors": [output_a_error, output_b_error],
        "completed": [completed_a, completed_b],
        "output_generation": [output_a_generation, output_b_generation],
    }


def _group_values(data):
    if len(data) != _CONTROL_RECORD.size + _GROUP_TAIL.size + 2 * _GROUP_PORT.size:
        raise RuntimeError("Invalid brickphyr.control-group/1 record length")
    (
        observed_ms, command_ms, deadline_ms,
        phase, request, port_mask, coast_mask,
        issue_error, duty_a, duty_b,
        generation_a, generation_b, group_read_error,
        requested_ms, completed_ms, group_deadline_ms,
        group_request, group_mask, applied_mask,
        group_error, cleanup_error, group_flags,
    ) = _GROUP_TAIL.unpack_from(data, _CONTROL_RECORD.size)
    ports = []
    offset = _CONTROL_RECORD.size + _GROUP_TAIL.size
    for port in range(2):
        (
            group_generation, group_port_request, group_port_error, port_cleanup_error,
            applied_ms, output_generation, output_request, output_completed,
            output_flags, duty_permille, output_error,
        ) = _GROUP_PORT.unpack_from(data, offset + port * _GROUP_PORT.size)
        ports.append({
            "group_generation": group_generation,
            "group_request": group_port_request,
            "group_error": group_port_error,
            "cleanup_error": port_cleanup_error,
            "applied_ms": applied_ms,
            "generation": output_generation,
            "request": output_request,
            "completed": output_completed,
            "mode": output_flags & 0xff,
            "valid": bool(output_flags & (1 << 8)),
            "pending": bool(output_flags & (1 << 9)),
            "owner": (output_flags >> 16) & 0xff,
            "duty_permille": duty_permille,
            "error": output_error,
        })
    return {
        "input": _control_values(data[:_CONTROL_RECORD.size]),
        "observed_ms": observed_ms,
        "command_ms": command_ms,
        "deadline_ms": deadline_ms,
        "phase": phase,
        "request": request,
        "port_mask": port_mask,
        "coast_mask": coast_mask,
        "issue_error": issue_error,
        "command_duty": [duty_a, duty_b],
        "command_generation": [generation_a, generation_b],
        "group_read_error": group_read_error,
        "group": {
            "requested_ms": requested_ms,
            "completed_ms": completed_ms,
            "deadline_ms": group_deadline_ms,
            "request": group_request,
            "port_mask": group_mask,
            "applied_mask": applied_mask,
            "error": group_error,
            "cleanup_error": cleanup_error,
            "pending": bool(group_flags & 1),
            "active": bool(group_flags & 2),
        },
        "ports": ports,
    }


_DECODERS = {
    _CONTROL_SCHEMA: (_CONTROL_RECORD.size, _control_values),
    _GROUP_SCHEMA: (_CONTROL_RECORD.size + _GROUP_TAIL.size + 2 * _GROUP_PORT.size,
                    _group_values),
}


async def collect(client, log, session, counts):
    info = await checked(client, Info())
    if info.size == 0:
        raise RuntimeError("Recording not initialized")
    if not 0 < info.generation < 1 << 32:
        raise RuntimeError("Invalid recording metadata")
    if not 0 < info.size <= 1024 or not 0 < info.capacity < 1 << 32 or (
        info.size * info.capacity > 32768
    ):
        raise RuntimeError("Invalid recording size or capacity")
    _range(info.first, info.next)
    if not info.record_schema or len(info.record_schema) > 47 or any(
        not 32 <= ord(char) <= 126 for char in info.record_schema
    ):
        raise RuntimeError("Invalid recording schema")
    decoder = _DECODERS.get(info.record_schema)
    if decoder and info.size != decoder[0]:
        raise RuntimeError(f"Invalid {info.record_schema} record length")

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
            },
        },
    )

    generation = info.generation
    first, next, active = info.first, info.next, info.active
    index = 0
    partial = bytearray()
    while True:
        if index < first:
            monitor.write_log(log, {"type": "gap", "from": index, "to": first})
            counts["gaps"] += 1
            index = first
            partial.clear()
        if not active and index >= next:
            status = await checked(client, application.Status())
            terminal_info = await checked(client, Info())
            if terminal_info.generation != generation:
                raise RuntimeError(
                    f"Recording generation changed from {generation} to {terminal_info.generation}"
                )
            monitor.write_log(
                log,
                {
                    "type": "application-end",
                    "state": status.state,
                    "result": status.result,
                    "fault": status.fault,
                    "cleanup": status.cleanup,
                },
            )
            return

        reply = await checked(client, Record(generation=generation, index=index, off=len(partial)))
        if reply.generation != generation:
            raise RuntimeError(
                f"Recording generation changed from {generation} to {reply.generation}"
            )
        _range(reply.first, reply.next)
        if reply.first < first or reply.next < next:
            raise RuntimeError("Recording index range moved backwards")
        first, next, active = reply.first, reply.next, reply.active
        if index < first:
            continue  # Discard the partial record before the next request.
        if index >= next:
            if reply.data:
                raise RuntimeError("Recording data outside retained range")
            if active:
                await asyncio.sleep(0.02)
            continue
        if not reply.data or len(reply.data) > 256 or len(reply.data) > info.size - len(partial):
            raise RuntimeError("Invalid recording fragment")
        partial.extend(reply.data)
        if len(partial) == info.size:
            record = {"type": "record", "index": index, "data": partial.hex()}
            if decoder:
                record["values"] = decoder[1](partial)
            monitor.write_log(log, record)
            counts["records"] += 1
            index += 1
            partial.clear()


async def _record(args, log):
    session = str(uuid.uuid4())
    failure = None
    counts = {"records": 0, "gaps": 0}
    try:
        async with connection(ble=args.ble, port=args.port, serial_number=args.serial) as (
            client,
            _,
        ):
            await collect(client, log, session, counts)
    except BaseException as error:
        failure = error
        raise
    finally:
        end = {
            "type": "end",
            "session": session,
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


def run(args):
    with monitor.recording(args.file) as log:
        asyncio.run(_record(args, log))
