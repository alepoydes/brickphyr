# SPDX-License-Identifier: Apache-2.0
"""Pure binary decoders for retained application records."""

import struct

from . import bench_codec, capture_codec

END_REASONS = {2: "trigger", 3: "full", 4: "duration", 5: "cancelled", 6: "error"}

_CONTROL_SCHEMA = "brickphyr.control-input/1"
_CONTROL_RECORD = struct.Struct("<8q10i6I2q4i4I")
_CONTROL_STEP = struct.Struct("<qq4I")
_LOOP_SCHEMA = "brickphyr.control-loop/1"
_GROUP_SCHEMA = "brickphyr.control-group/1"
_GROUP_SCHEMA_V2 = "brickphyr.control-group/2"
_GROUP_TAIL = struct.Struct("<3q4I3i2Ii3q3I2iI")
_GROUP_PORT = struct.Struct("<2I2iq4I2i")
_GROUP_TAIL_V2 = struct.Struct(_GROUP_TAIL.format + "2I")
_GROUP_PORT_V2 = struct.Struct(_GROUP_PORT.format + "2qiB3x")
_ACTUATION_SCHEMA = "brickphyr.control-actuation/1"
_ACTUATION_SCHEMA_V2 = "brickphyr.control-actuation/2"
_ACTUATION_TAIL = struct.Struct("<2q5I3i2I3q3I2iI")
_ACTUATION_PORT = struct.Struct("<2I2i")
_ACTUATION_TAIL_V2 = struct.Struct(_ACTUATION_TAIL.format + "2I")
_ACTUATION_PORT_V2 = struct.Struct(_ACTUATION_PORT.format + "2qiB3x")


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


def _group_values(data, version):
    (
        observed_ms, command_ms, deadline_ms,
        phase, request, port_mask, coast_mask,
        issue_error, duty_a, duty_b,
        generation_a, generation_b, group_read_error,
        requested_ms, completed_ms, group_deadline_ms,
        group_request, group_mask, applied_mask,
        group_error, cleanup_error, group_flags,
    ) = _GROUP_TAIL.unpack_from(data, _CONTROL_RECORD.size)
    tail = _GROUP_TAIL_V2 if version == 2 else _GROUP_TAIL
    port_record = _GROUP_PORT_V2 if version == 2 else _GROUP_PORT
    group = {
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
    }
    if version == 2:
        group["cleanup_mask"], group["timestamp_resolution_us"] = struct.unpack_from(
            "<2I", data, _CONTROL_RECORD.size + _GROUP_TAIL.size
        )
    ports = []
    offset = _CONTROL_RECORD.size + tail.size
    for port in range(2):
        (
            group_generation, group_port_request, group_port_error, port_cleanup_error,
            applied_ms, output_generation, output_request, output_completed,
            output_flags, duty_permille, output_error,
        ) = _GROUP_PORT.unpack_from(data, offset + port * port_record.size)
        member = {
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
        }
        if version == 2:
            (member["group_applied_us"], member["group_cleanup_us"],
             member["group_duty_permille"], member["group_mode"]) = struct.unpack_from(
                "<2qiB", data, offset + port * port_record.size + _GROUP_PORT.size
            )
        ports.append(member)
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
        "group": group,
        "ports": ports,
    }


def _step_values(data):
    scheduled, started, sequence, skipped, revision, size = struct.unpack_from("<qq4I", data)
    port_mask, _reserved = struct.unpack_from("<2I", data, 208)
    return {
        "scheduled_us": scheduled, "started_us": started,
        "sequence": sequence, "skipped": skipped,
        "parameter_revision": revision, "parameter_size": size, "port_mask": port_mask,
        "input": _control_values(data[32:208]),
    }


def _actuation_values(data, version):
    (
        command_ms, deadline_ms, phase, port_mask, coast_mask, request, previous_sequence,
        issue_error, duty_a, duty_b, generation_a, generation_b,
        requested_ms, completed_ms, previous_deadline_ms, previous_request,
        previous_mask, applied_mask, error, cleanup_error, flags,
    ) = _ACTUATION_TAIL.unpack_from(data, _CONTROL_RECORD.size)
    tail = _ACTUATION_TAIL_V2 if version == 2 else _ACTUATION_TAIL
    port_record = _ACTUATION_PORT_V2 if version == 2 else _ACTUATION_PORT
    previous = {"control_sequence": previous_sequence, "request": previous_request,
                "requested_ms": requested_ms, "completed_ms": completed_ms,
                "deadline_ms": previous_deadline_ms, "port_mask": previous_mask,
                "applied_mask": applied_mask, "error": error,
                "cleanup_error": cleanup_error, "pending": bool(flags & 1),
                "active": bool(flags & 2)}
    if version == 2:
        previous["cleanup_mask"], previous["timestamp_resolution_us"] = struct.unpack_from(
            "<2I", data, _CONTROL_RECORD.size + _ACTUATION_TAIL.size
        )
    ports = []
    offset = _CONTROL_RECORD.size + tail.size
    for port in range(2):
        generation, port_request, port_error, port_cleanup = _ACTUATION_PORT.unpack_from(
            data, offset + port * port_record.size
        )
        member = dict(generation=generation, request=port_request,
                      error=port_error, cleanup_error=port_cleanup)
        if version == 2:
            (member["applied_us"], member["cleanup_us"], member["duty_permille"],
             member["mode"]) = struct.unpack_from(
                "<2qiB", data, offset + port * port_record.size + _ACTUATION_PORT.size
            )
        ports.append(member)
    previous["ports"] = ports
    return {
        "input": _control_values(data[:_CONTROL_RECORD.size]),
        "phase": phase,
        "command": {"issued_ms": command_ms, "deadline_ms": deadline_ms,
                    "port_mask": port_mask, "coast_mask": coast_mask,
                    "request": request, "error": issue_error,
                    "duty_permille": [duty_a, duty_b],
                    "generation": [generation_a, generation_b]},
        "previous": previous,
    }


def _loop_values(data):
    if len(data) != _CONTROL_RECORD.size + 8:
        raise RuntimeError("Invalid brickphyr.control-loop/1 record length")
    port_mask, _reserved = struct.unpack_from("<2I", data, _CONTROL_RECORD.size)
    return {"input": _control_values(data[:_CONTROL_RECORD.size]),
            "port_mask": port_mask}


def control_step(data, parameter_size):
    if len(data) != _CONTROL_STEP.size:
        raise RuntimeError("Invalid control step metadata length")
    scheduled, started, sequence, skipped, revision, size = _CONTROL_STEP.unpack(data)
    if size != parameter_size:
        raise RuntimeError("Control step parameter size differs from recording")
    return {
        "scheduled_us": scheduled, "started_us": started,
        "sequence": sequence, "skipped": skipped,
        "parameter_revision": revision, "parameter_size": size,
    }


_DECODERS = {
    _ACTUATION_SCHEMA: (
        _CONTROL_RECORD.size + _ACTUATION_TAIL.size + 2 * _ACTUATION_PORT.size,
        lambda data: _actuation_values(data, 1),
    ),
    _ACTUATION_SCHEMA_V2: (
        _CONTROL_RECORD.size + _ACTUATION_TAIL_V2.size + 2 * _ACTUATION_PORT_V2.size,
        lambda data: _actuation_values(data, 2),
    ),
    "brickphyr.control-step/1": (216, _step_values),
    _LOOP_SCHEMA: (_CONTROL_RECORD.size + 8, _loop_values),
    _CONTROL_SCHEMA: (_CONTROL_RECORD.size, _control_values),
    _GROUP_SCHEMA: (_CONTROL_RECORD.size + _GROUP_TAIL.size + 2 * _GROUP_PORT.size,
                    lambda data: _group_values(data, 1)),
    _GROUP_SCHEMA_V2: (_CONTROL_RECORD.size + _GROUP_TAIL_V2.size + 2 * _GROUP_PORT_V2.size,
                       lambda data: _group_values(data, 2)),
}


def check_size(schema, size, config=None, *, variable=False):
    if schema == bench_codec.SCHEMA:
        if variable:
            raise RuntimeError("brickphyr.bench/1 requires fixed-size records")
        if config is not None:
            bench_codec.check_size(size, config)
        return
    if schema == bench_codec.SCHEMA_V2:
        if not variable:
            raise RuntimeError("brickphyr.bench/2 requires variable-size records")
        if config is not None:
            bench_codec.check_size(size, config, schema=schema)
        return
    if schema == capture_codec.SCHEMA:
        if config is not None:
            expected = capture_codec._layout(config)[4]
            invalid = size < expected if variable else size != expected
            if invalid:
                raise RuntimeError(f"Invalid {schema} record length")
        return
    decoder = _DECODERS.get(schema)
    if decoder and (size < decoder[0] if variable else size != decoder[0]):
        raise RuntimeError(f"Invalid {schema} record length")


def decode(schema, data, config=b"") -> dict | None:
    if schema == bench_codec.SCHEMA:
        return bench_codec.decode(data, config)
    if schema == bench_codec.SCHEMA_V2:
        return bench_codec.decode(data, config, schema=schema)
    if schema == capture_codec.SCHEMA:
        return capture_codec.decode(data, config)
    check_size(schema, len(data), config)
    decoder = _DECODERS.get(schema)
    return decoder[1](data) if decoder else None
