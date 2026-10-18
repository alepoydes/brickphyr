# SPDX-License-Identifier: Apache-2.0
"""Application fields appended to an existing capture/2 control row."""

import re
import struct

from . import capture_codec
from .capture_layout import CONFIG_SIZE


SCHEMA = "brickphyr.capture+app/1"
MAX_FIELDS = 8
MAX_NAME = 23
MAX_CONFIG_SIZE = CONFIG_SIZE + 1 + MAX_FIELDS * (2 + MAX_NAME)
_TYPES = {kind: struct.Struct("<" + kind) for kind in "bBhHiIf"}
_NAME = re.compile(rb"[A-Za-z][A-Za-z0-9_]*\Z")


def layout(config):
    """Return the base row length, selected fields, and complete row length."""
    if not CONFIG_SIZE + 1 < len(config) <= MAX_CONFIG_SIZE:
        raise RuntimeError("Invalid application capture configuration length")
    base_size = capture_codec._layout_v2(config[:CONFIG_SIZE])[4]
    count = config[CONFIG_SIZE]
    if not 1 <= count <= MAX_FIELDS:
        raise RuntimeError("Invalid application field count")
    offset = CONFIG_SIZE + 1
    fields = []
    names = set()
    for _ in range(count):
        if offset + 2 > len(config):
            raise RuntimeError("Truncated application field descriptor")
        kind, length = config[offset], config[offset + 1]
        offset += 2
        if not 1 <= length <= MAX_NAME or offset + length > len(config):
            raise RuntimeError("Invalid application field name length")
        name = bytes(config[offset:offset + length])
        offset += length
        field = _TYPES.get(chr(kind))
        if field is None or not _NAME.fullmatch(name) or name in names:
            raise RuntimeError("Invalid application field descriptor")
        names.add(name)
        fields.append((name.decode("ascii"), field))
    if offset != len(config):
        raise RuntimeError("Extra application field descriptors")
    return base_size, tuple(fields), base_size + sum(field.size for _, field in fields)


def config_with_fields(base, fields):
    """Build immutable metadata once; rows contain only field values."""
    capture_codec._layout_v2(base)
    if not 1 <= len(fields) <= MAX_FIELDS:
        raise ValueError("Expected 1..8 application fields")
    result = bytearray(base)
    result.append(len(fields))
    for name, kind in fields:
        encoded = name.encode("ascii")
        if kind not in _TYPES or not 1 <= len(encoded) <= MAX_NAME or not _NAME.fullmatch(encoded):
            raise ValueError("Invalid application field")
        result.extend((ord(kind), len(encoded)))
        result.extend(encoded)
    layout(result)
    return bytes(result)


def decode(data, config):
    base_size, fields, row_size = layout(config)
    if len(data) != row_size:
        raise RuntimeError("Invalid application capture row length")
    values = capture_codec.decode(data[:base_size], config[:CONFIG_SIZE],
                                  schema=capture_codec.SCHEMA_V2)
    offset = base_size
    app = {}
    for name, field in fields:
        app[name] = field.unpack_from(data, offset)[0]
        offset += field.size
    values["app"] = app
    return values
