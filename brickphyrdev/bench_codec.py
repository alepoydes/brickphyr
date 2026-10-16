# SPDX-License-Identifier: Apache-2.0
"""Decode fixed-size and compact bench schedule/observation records."""

import struct

from . import capture_codec

SCHEMA = "brickphyr.bench/1"
SCHEMA_V2 = "brickphyr.bench/2"

_CONFIG = struct.Struct("<12I")
_STEP = struct.Struct("<3I6h")
_KIND = struct.Struct("<I")
_BEGIN = struct.Struct("<Iq")
_GROUP = struct.Struct("<IIqqIiii4I")
_GROUP_PORT = struct.Struct("<IIiIqqii")
_END = struct.Struct("<Iqii4I")
_PORTS = "ABCDEF"


def _layout(config, schema=SCHEMA):
    if not isinstance(config, bytes | bytearray) or not _CONFIG.size <= len(config) <= 1024:
        raise RuntimeError(f"Invalid {schema} configuration length")
    if (len(config) - _CONFIG.size) % _STEP.size:
        raise RuntimeError(f"Invalid {schema} configuration length")
    (source_mask, period_ms, duration_ms, resolution_us, port_mask, step_count,
     *generations) = _CONFIG.unpack_from(config)
    if not step_count or len(config) != _CONFIG.size + step_count * _STEP.size:
        raise RuntimeError(f"Invalid {schema} configuration length")
    if not port_mask or port_mask >> len(_PORTS):
        raise RuntimeError(f"Invalid {schema} port mask")
    # The first four fields are exactly the capture/1 configuration.
    snapshot_size = capture_codec._layout(config[:16])[4]
    steps = []
    previous_offset = -1
    for index in range(step_count):
        offset_ms, duration, coast_mask, *duties = _STEP.unpack_from(
            config, _CONFIG.size + index * _STEP.size
        )
        if offset_ms <= previous_offset or coast_mask & ~port_mask:
            raise RuntimeError(f"Invalid {schema} step")
        if coast_mask != port_mask and not duration:
            raise RuntimeError(f"Invalid {schema} step duration")
        if any(abs(duty) > 1000 or duty and not (port_mask & (1 << port)) or
               duty and coast_mask & (1 << port) for port, duty in enumerate(duties)):
            raise RuntimeError(f"Invalid {schema} step duty")
        steps.append(dict(offset_ms=offset_ms, duration_ms=duration, coast_mask=coast_mask,
                          duty_permille=dict(zip(_PORTS, duties, strict=True))))
        previous_offset = offset_ms
    size = max(_KIND.size + snapshot_size, _GROUP.size + _GROUP_PORT.size * port_mask.bit_count(),
               _END.size)
    return dict(source_mask=source_mask, period_ms=period_ms, duration_ms=duration_ms,
                resolution_us=resolution_us, port_mask=port_mask, generations=generations,
                steps=steps, snapshot_size=snapshot_size, size=size)


def check_size(size, config, *, schema=SCHEMA):
    if size != _layout(config, schema)["size"]:
        raise RuntimeError(f"Invalid {schema} record length")


def _padding(data, used, schema):
    if schema == SCHEMA_V2:
        if len(data) != used:
            raise RuntimeError(f"Invalid {schema} record length")
    elif any(data[used:]):
        raise RuntimeError(f"Nonzero {schema} slot padding")


def decode(data, config, *, schema=SCHEMA):
    layout = _layout(config, schema)
    if len(data) < _KIND.size:
        raise RuntimeError(f"Invalid {schema} record length")
    kind = _KIND.unpack_from(data)[0]
    if kind > 3:
        raise RuntimeError(f"Invalid {schema} slot kind")
    expected = (
        _KIND.size + layout["snapshot_size"],
        _GROUP.size + _GROUP_PORT.size * layout["port_mask"].bit_count(),
        _BEGIN.size,
        _END.size,
    )[kind] if schema == SCHEMA_V2 else layout["size"]
    if len(data) != expected:
        raise RuntimeError(f"Invalid {schema} record length")
    if kind == 0:
        used = _KIND.size + layout["snapshot_size"]
        values = capture_codec.decode(data[_KIND.size:used], config[:16])
        _padding(data, used, schema)
        return {"kind": "sample", **values}
    if kind == 1:
        (kind, action, scheduled_us, submitted_us, request, admission_error, error,
         cleanup_error, flags, applied_mask, cleanup_mask, resolution_us) = _GROUP.unpack_from(data)
        port_mask = layout["port_mask"]
        if not 1 <= action <= len(layout["steps"]) or flags & ~3 or (
            applied_mask | cleanup_mask
        ) & ~port_mask or not resolution_us:
            raise RuntimeError(f"Invalid {schema} group result")
        ports = {}
        offset = _GROUP.size
        for port, name in enumerate(_PORTS):
            if not port_mask & (1 << port):
                continue
            (generation, port_request, duty, mode, applied_us, cleanup_us,
             port_error, port_cleanup_error) = _GROUP_PORT.unpack_from(data, offset)
            ports[name] = dict(generation=generation, request=port_request,
                               duty_permille=duty, mode=mode, applied_us=applied_us,
                               cleanup_us=cleanup_us, error=port_error,
                               cleanup_error=port_cleanup_error)
            offset += _GROUP_PORT.size
        _padding(data, offset, schema)
        return dict(kind="group", action=action, scheduled_us=scheduled_us,
                    submitted_us=submitted_us, request=request, admission_error=admission_error,
                    error=error, cleanup_error=cleanup_error, pending=bool(flags & 1),
                    active=bool(flags & 2), applied_mask=applied_mask, cleanup_mask=cleanup_mask,
                    resolution_us=resolution_us, ports=ports)
    if kind == 2:
        _kind, origin_us = _BEGIN.unpack_from(data)
        _padding(data, _BEGIN.size, schema)
        return {"kind": "begin", "origin_us": origin_us}
    if kind == 3:
        (_kind, end_us, error, cleanup_error, missed, planned_samples,
         written_samples, attempted_actions) = _END.unpack_from(data)
        _padding(data, _END.size, schema)
        return dict(kind="end", end_us=end_us, error=error, cleanup_error=cleanup_error,
                    missed=missed, planned_samples=planned_samples,
                    written_samples=written_samples, attempted_actions=attempted_actions)
    raise RuntimeError(f"Invalid {schema} slot kind")
