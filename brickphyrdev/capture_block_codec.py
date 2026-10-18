# SPDX-License-Identifier: Apache-2.0
"""Bounded lossless blocks of capture observation rows.

The caller supplies the immutable capture configuration. Each block is independently
decodable and carries full values for every field changed since the previous row.
"""

import re
import struct

from . import capture_app_codec, capture_codec
from .capture_layout import CONFIG_SIZE, DELTA_COUNTERS, IMU_SHARED_FIELDS, KINDS, SOURCE_KINDS

_HEADER = struct.Struct("<4sHH")
_MAGIC = b"BCB3"
_COMPACT_FIRST_MAGIC = b"BCB4"
_MAX_ROWS = 255
_MAX_RECORD_BYTES = 1024
MAX_TRANSFER_BYTES = 2048


def _field_formats(config):
    # The decoder validates selection and length before any block is inspected.
    app_fields = ()
    if len(config) != CONFIG_SIZE:
        _base_size, app_fields, row_size = capture_app_codec.layout(config)
    else:
        row_size = capture_codec._layout_v2(config)[4]
    mask = int.from_bytes(config[:4], "little")
    header = capture_codec._HEADER.format.removeprefix("<")
    groups = re.findall(r"(\d*)([bBhHiIlLqQ])", header)
    if "".join(count + fmt for count, fmt in groups) != header:
        raise ValueError("Unsupported capture header format")
    formats = tuple(fmt for count, fmt in groups for _ in range(int(count or 1)))
    for bit, kind in enumerate(SOURCE_KINDS):
        if mask & (1 << bit):
            selection = config[capture_codec._CONFIG.size + bit]
            optional = 0
            for selector, _name, fmt, _binding in KINDS[kind][2]:
                if selector is None or selection & (1 << optional):
                    formats += (fmt,)
                optional += selector is not None
    formats += tuple(field.format.removeprefix("<") for _, field in app_fields)
    if sum(struct.calcsize("<" + fmt) for fmt in formats) != row_size:
        raise ValueError("Capture field layout differs from payload length")
    return tuple(struct.Struct("<" + fmt) for fmt in formats)


def _aliases(config, count):
    mask = int.from_bytes(config[:4], "little")
    aliases = [-1] * count
    if (mask & 3) == 3:
        raw_optional = config[capture_codec._CONFIG.size]
        raw_count = optional = 0
        for selector, *_rest in KINDS[0][2]:
            if selector is None:
                raw_count += 1
            else:
                raw_count += bool(raw_optional & (1 << optional))
                optional += 1
        calibrated_start = 4 + raw_count
        for index in range(IMU_SHARED_FIELDS):
            aliases[calibrated_start + index] = 4 + index
    return aliases


def _delta_flags(config):
    mask = int.from_bytes(config[:4], "little")
    flags = [True, True, True, True]
    for bit, kind in enumerate(SOURCE_KINDS):
        if not mask & (1 << bit):
            continue
        selection = config[capture_codec._CONFIG.size + bit]
        optional = 0
        for selector, name, fmt, _binding in KINDS[kind][2]:
            if selector is None or selection & (1 << optional):
                flags.append(fmt == "q" and name.endswith(("_us", "_ms")) or
                             fmt == "I" and name in DELTA_COUNTERS)
            optional += selector is not None
    if len(config) != CONFIG_SIZE:
        _base_size, fields, _row_size = capture_app_codec.layout(config)
        flags.extend(field.format[-1] in "iI" for _, field in fields)
    return flags


def _delta_encoding(old, new):
    signed = len(new) == 8
    delta = int.from_bytes(new, "little", signed=signed) - int.from_bytes(
        old, "little", signed=signed)
    for size in range(1, len(new)):
        if -(1 << (size * 8 - 1)) <= delta < 1 << (size * 8 - 1):
            return bytes((size,)) + delta.to_bytes(size, "little", signed=True)
    return b"\0" + new


def _split(row, fields):
    parts = []
    offset = 0
    for field in fields:
        parts.append(row[offset:offset + field.size])
        offset += field.size
    return parts


def _indices(mask, count):
    if len(mask) != (count + 7) // 8 or count % 8 and mask[-1] >> (count % 8):
        raise ValueError("Invalid field mask length or padding")
    return [index for index in range(count) if mask[index // 8] & (1 << (index % 8))]


def encode(rows, config):
    """Consume 1..255 capture rows online into one block of at most 1024 bytes."""
    fields = _field_formats(config)
    aliases = _aliases(config, len(fields))
    deltas = _delta_flags(config)
    size = sum(field.size for field in fields)
    output = bytearray(_HEADER.pack(_MAGIC, 0, size))
    previous = None
    count = 0
    for source in rows:
        if count == _MAX_ROWS:
            raise ValueError("Too many rows in block")
        if len(source) != size:
            raise ValueError("Invalid capture/2 row length")
        row = _split(source, fields)
        if any(row[index] != row[alias] for index, alias in enumerate(aliases) if alias >= 0):
            raise ValueError("Shared IMU metadata differs within one row")
        if previous is None:
            addition = b"".join(value for index, value in enumerate(row) if aliases[index] < 0)
        else:
            mask = bytearray((len(fields) + 7) // 8)
            changed = bytearray()
            for index, (old, new) in enumerate(zip(previous, row, strict=True)):
                if old != new:
                    mask[index // 8] |= 1 << (index % 8)
                    if aliases[index] < 0:
                        changed.extend(_delta_encoding(old, new) if deltas[index] else new)
            addition = mask + changed
        if len(output) + len(addition) > _MAX_RECORD_BYTES:
            raise ValueError("Block exceeds 1024 bytes")
        output.extend(addition)
        previous = row
        count += 1
    if not count:
        raise ValueError("Empty block")
    struct.pack_into("<H", output, 4, count)
    return bytes(output)


def _decode(block, fields, aliases, deltas):
    size = sum(field.size for field in fields)
    if not _HEADER.size <= len(block) <= MAX_TRANSFER_BYTES:
        raise ValueError("Invalid block length")
    magic, count, row_size = _HEADER.unpack_from(block)
    if magic not in (_MAGIC, _COMPACT_FIRST_MAGIC) or not 1 <= count <= _MAX_ROWS or row_size != size:
        raise ValueError("Invalid block header")
    offset = _HEADER.size

    def take(length):
        nonlocal offset
        if offset + length > len(block):
            raise ValueError("Truncated block")
        result = block[offset:offset + length]
        offset += length
        return result

    if magic == _COMPACT_FIRST_MAGIC:
        table_size = (len(fields) + 1) // 2
        table = take(table_size)
        if len(fields) % 2 and table[-1] >> 4:
            raise ValueError("Invalid first-row padding nibble")
        full_size, compact_size = 0, table_size
        previous = []
        for index, field in enumerate(fields):
            width = table[index // 2] >> (4 * (index % 2)) & 15
            if aliases[index] >= 0:
                if width:
                    raise ValueError("Aliased first-row field has data")
                previous.append(None)
                continue
            full_size += field.size
            if field.size > 15 or width > field.size:
                raise ValueError("Invalid first-row field width")
            part = take(width)
            if width and not part[-1]:
                raise ValueError("Noncanonical first-row field width")
            compact_size += width
            previous.append(bytes(part) + bytes(field.size - width))
        if compact_size >= full_size:
            raise ValueError("Noncanonical first-row block")
    else:
        previous = [take(field.size) if aliases[index] < 0 else None
                    for index, field in enumerate(fields)]
    for index, alias in enumerate(aliases):
        if alias >= 0:
            previous[index] = previous[alias]
    rows = [b"".join(previous)]
    for _ in range(count - 1):
        changed = _indices(take((len(fields) + 7) // 8), len(fields))
        row = previous.copy()
        for index in changed:
            if aliases[index] >= 0:
                value = row[aliases[index]]
            elif deltas[index]:
                width = fields[index].size
                marker = take(1)[0]
                if marker >= width:
                    raise ValueError("Invalid delta length")
                if marker:
                    old = int.from_bytes(previous[index], "little", signed=width == 8)
                    difference = int.from_bytes(take(marker), "little", signed=True)
                    try:
                        value = (old + difference).to_bytes(width, "little", signed=width == 8)
                    except OverflowError as error:
                        raise ValueError("Delta exceeds field range") from error
                else:
                    value = take(width)
            else:
                value = take(fields[index].size)
            if value == previous[index]:
                raise ValueError("Unchanged field marked changed")
            row[index] = value
        if any(row[index] != row[alias] for index, alias in enumerate(aliases) if alias >= 0):
            raise ValueError("Shared IMU metadata differs within one row")
        rows.append(b"".join(row))
        previous = row
    if offset != len(block):
        raise ValueError("Extra block bytes")
    return rows


def decode(block, config):
    """Return exact capture/2 bytes; reject truncated, extra or malformed data."""
    fields = _field_formats(config)
    return _decode(block, fields, _aliases(config, len(fields)), _delta_flags(config))


def decode_control(block, capture_config):
    """Return exact 32-byte control steps followed by capture/2 rows."""
    capture_fields = _field_formats(capture_config)
    step_fields = (struct.Struct("<q"),) * 2 + (struct.Struct("<I"),) * 4
    aliases = [-1] * len(step_fields) + [
        alias + len(step_fields) if alias >= 0 else -1
        for alias in _aliases(capture_config, len(capture_fields))
    ]
    deltas = [True] * len(step_fields) + _delta_flags(capture_config)
    return _decode(block, step_fields + capture_fields, aliases, deltas)
