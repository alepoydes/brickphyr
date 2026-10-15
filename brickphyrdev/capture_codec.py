# SPDX-License-Identifier: Apache-2.0
"""Decode the compact, explicitly packed brickphyr.capture/1 records."""

import struct

SCHEMA = "brickphyr.capture/1"

_CONFIG = struct.Struct("<4I")
_HEADER = struct.Struct("<3qI")
_IMU = struct.Struct("<2q3Ii6i")
_IMU_CALIBRATION = struct.Struct("<IiI")
_BATTERY = struct.Struct("<2q4Ii2I")
_ENCODER = struct.Struct("<2q2Ii")
_MOTOR = struct.Struct("<2q5I3i4B")
_SOURCES = ("imu", "imu.calibrated", "battery", "encoder.A", "motor.A",
            "encoder.B", "motor.B", "encoder.C", "motor.C", "encoder.D", "motor.D",
            "encoder.E", "motor.E", "encoder.F", "motor.F")
_SIZES = (_IMU.size, _IMU.size + _IMU_CALIBRATION.size, _BATTERY.size) + (
    _ENCODER.size, _MOTOR.size
) * 6


def _layout(config):
    if len(config) != _CONFIG.size:
        raise RuntimeError("Invalid brickphyr.capture/1 configuration length")
    mask, period_ms, duration_ms, resolution_us = _CONFIG.unpack(config)
    if not mask or mask >> len(_SOURCES) or not period_ms or not resolution_us:
        raise RuntimeError("Invalid brickphyr.capture/1 configuration")
    size = _HEADER.size + sum(size for bit, size in enumerate(_SIZES) if mask & (1 << bit))
    return mask, period_ms, duration_ms, resolution_us, size


def check_size(size, config):
    if size != _layout(config)[4]:
        raise RuntimeError("Invalid brickphyr.capture/1 record length")


def _imu(data, offset, calibrated):
    (sample_us, read_us, sequence, missed, failures, raw_error,
     ax, ay, az, gx, gy, gz) = _IMU.unpack_from(data, offset)
    value = {
        "sample_us": sample_us, "read_us": read_us, "sequence": sequence,
        "missed": missed, "failures": failures,
        "acceleration_um_s2": [ax, ay, az], "angular_velocity_mdps": [gx, gy, gz],
    }
    if calibrated:
        epoch, error, flags = _IMU_CALIBRATION.unpack_from(data, offset + _IMU.size)
        value.update(raw_error=raw_error, epoch=epoch, error=error,
                     ready=bool(flags & 1), stationary=bool(flags & 2))
    else:
        value["error"] = raw_error
    return value


def _battery(data, offset):
    (sample_ms, read_ms, voltage_mv, current_ma, sequence, failures,
     error, average_mv, flags) = _BATTERY.unpack_from(data, offset)
    return {
        "sample_ms": sample_ms, "read_ms": read_ms, "voltage_mv": voltage_mv,
        "current_ma": current_ma, "sequence": sequence, "failures": failures,
        "error": error, "average_mv": average_mv,
        "low": bool(flags & 1), "critical": bool(flags & 2),
    }


def _encoder(data, offset):
    sample_ms, angle_mdeg, generation, sequence, error = _ENCODER.unpack_from(data, offset)
    return dict(sample_ms=sample_ms, angle_mdeg=angle_mdeg, generation=generation,
                sequence=sequence, error=error)


def _motor(data, offset):
    (requested_ms, applied_ms, generation, request, completed, request_generation,
     output_generation, output_error, cleanup_error, duty_permille,
     mode, valid, pending, owner) = _MOTOR.unpack_from(data, offset)
    return {
        "requested_ms": requested_ms, "applied_ms": applied_ms,
        "generation": generation, "request": request, "completed": completed,
        "request_generation": request_generation, "output_generation": output_generation,
        "output_error": output_error, "cleanup_error": cleanup_error,
        "duty_permille": duty_permille, "mode": mode, "valid": bool(valid),
        "pending": bool(pending), "owner": owner,
    }


def decode(data, config):
    mask, _period_ms, _duration_ms, resolution_us, size = _layout(config)
    if len(data) != size:
        raise RuntimeError("Invalid brickphyr.capture/1 record length")
    scheduled_us, started_us, completed_us, skipped = _HEADER.unpack_from(data)
    sources = {}
    offset = _HEADER.size
    for bit, name in enumerate(_SOURCES):
        if not mask & (1 << bit):
            continue
        if bit < 2:
            value = _imu(data, offset, calibrated=bit == 1)
        elif bit == 2:
            value = _battery(data, offset)
        elif bit % 2:
            value = _encoder(data, offset)
        else:
            value = _motor(data, offset)
        sources[name] = value
        offset += _SIZES[bit]
    return {
        "scheduled_us": scheduled_us, "started_us": started_us,
        "completed_us": completed_us, "skipped": skipped,
        "resolution_us": resolution_us, "sources": sources,
    }
