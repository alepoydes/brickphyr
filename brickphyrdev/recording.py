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
    if info.record_schema == _CONTROL_SCHEMA and info.size != _CONTROL_RECORD.size:
        raise RuntimeError("Invalid brickphyr.control-input/1 record length")

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
            if info.record_schema == _CONTROL_SCHEMA:
                record["values"] = _control_values(partial)
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
