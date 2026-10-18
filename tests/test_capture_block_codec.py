# SPDX-License-Identifier: Apache-2.0
"""Independent block decode and corrupt-length boundaries for capture/2 rows."""

import ctypes
import io
import shutil
import struct
import subprocess
from pathlib import Path

import cbor2
import pytest

from brickphyrdev import bench_codec, capture_block_codec, capture_codec, record_codec
from brickphyrdev.records import Recording


def _native_encode(tmp_path, rows, config, *, control=False, capacity=1024,
                   compact_first=False):
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("host C compiler unavailable")
    root = Path(__file__).resolve().parents[1]
    library = tmp_path / "telemetry_block.so"
    subprocess.run((compiler, "-shared", "-fPIC", "-O2", "-I", str(root / "include"),
                    str(root / "src/telemetry_block.c"), "-o", str(library)), check=True)

    class WireField(ctypes.Structure):
        _fields_ = (("offset", ctypes.c_uint16), ("size", ctypes.c_uint8),
                    ("alias", ctypes.c_int16), ("delta", ctypes.c_uint8))

    class Block(ctypes.Structure):
        _fields_ = (("data", ctypes.c_void_p), ("previous", ctypes.c_void_p),
                    ("fields", ctypes.c_void_p), ("capacity", ctypes.c_size_t),
                    ("length", ctypes.c_size_t), ("row_size", ctypes.c_size_t),
                    ("field_count", ctypes.c_size_t), ("rows", ctypes.c_uint16),
                    ("compact_first", ctypes.c_bool))

    widths = [field.size for field in capture_block_codec._field_formats(config)]
    offset = 0
    aliases = capture_block_codec._aliases(config, len(widths))
    deltas = capture_block_codec._delta_flags(config)
    if control:
        widths = [8, 8, 4, 4, 4, 4] + widths
        aliases = [-1] * 6 + [alias + 6 if alias >= 0 else -1 for alias in aliases]
        deltas = [True] * 6 + deltas
    fields = (WireField * len(widths))()
    for index, width in enumerate(widths):
        fields[index] = WireField(offset, width, aliases[index], deltas[index])
        offset += width
    output = (ctypes.c_uint8 * capacity)()
    previous = (ctypes.c_uint8 * len(rows[0]))()
    state = Block()
    native = ctypes.CDLL(str(library))
    native.telemetry_block_init.argtypes = (ctypes.POINTER(Block), ctypes.c_void_p,
                                            ctypes.c_size_t, ctypes.c_void_p,
                                            ctypes.c_size_t, ctypes.c_void_p,
                                            ctypes.c_size_t, ctypes.c_bool)
    native.telemetry_block_append.argtypes = (ctypes.POINTER(Block), ctypes.c_void_p)
    assert native.telemetry_block_init(ctypes.byref(state), output, capacity, previous,
                                       len(rows[0]), fields, len(fields), compact_first) == 0
    for value in rows:
        source = (ctypes.c_uint8 * len(value)).from_buffer_copy(value)
        assert native.telemetry_block_append(ctypes.byref(state), source) == 0
    return bytes(output[:state.length])


def test_block_round_trip_and_corruption(tmp_path):
    # Battery voltage and motor duty are selected; every timestamp and validity
    # byte remains in the packed row, including fields that never change.
    selected = bytearray(15)
    selected[2] = 1
    selected[4] = 1
    config = struct.pack("<4I", (1 << 2) | (1 << 4), 20, 100, 1) + selected

    def row(time, voltage, duty, error=0, valid=1, sequence=7):
        return b"".join((
            struct.pack("<3qI", time, time + 2, time + 4, 0),
            struct.pack("<2q2Ii2I", time // 1000, time // 1000, sequence, 0, error, 0, voltage),
            struct.pack("<qIIii4Bi", time // 1000, 2, 3, 0, 0, 2, valid, 0, 1, duty),
        ))

    rows = [row(100_000, 7500, 0), row(100_000, 7500, 0),
            row(120_000, 7501, -1, -5, 0), row(140_000, 0xffffffff, 1000)]
    assert all(capture_codec.decode(value, config, schema=capture_codec.SCHEMA_V2)
               for value in rows)
    block = capture_block_codec.encode(iter(rows), config)
    assert _native_encode(tmp_path, rows, config) == block
    assert struct.unpack_from("<4sHH", block) == (b"BCB3", len(rows), len(rows[0]))
    assert capture_block_codec.decode(block, config) == rows
    extremes = [row(2**63 - 16, 7500, 0, sequence=0xffffffff),
                row(-2**63 + 16, 7500, 0, sequence=0)]
    assert capture_block_codec.decode(capture_block_codec.encode(extremes, config),
                                      config) == extremes
    assert _native_encode(tmp_path, extremes, config) == capture_block_codec.encode(
        extremes, config)
    compact = _native_encode(tmp_path, rows, config, compact_first=True)
    assert compact[:4] == b"BCB4" and len(compact) < len(block)
    assert capture_block_codec.decode(compact, config) == rows
    incompressible = [b"\xff" * len(rows[0])]
    assert _native_encode(tmp_path, incompressible, config, compact_first=True) == (
        _native_encode(tmp_path, incompressible, config)
    )
    large_rows = [row(100_000 + index * 15_000, 7000 + index, index,
                      sequence=index) for index in range(50)]
    large_block = _native_encode(tmp_path, large_rows, config, capacity=2048)
    assert 1024 < len(large_block) <= 2048
    assert capture_block_codec.decode(large_block, config) == large_rows
    with pytest.raises(ValueError, match="block length"):
        capture_block_codec.decode(large_block.ljust(2049, b"\0"), config)
    record_codec.check_size(capture_codec.SCHEMA_V3, 1024, config, variable=True)
    assert record_codec.decode(capture_codec.SCHEMA_V3, block, config)["samples"] == [
        capture_codec.decode(value, config, schema=capture_codec.SCHEMA_V2)
        for value in rows
    ]
    schedule = config + struct.pack("<8I", 1, 1, 7, 0, 0, 0, 0, 0) + struct.pack(
        "<3I6h", 0, 50, 0, 200, 0, 0, 0, 0, 0)
    record_codec.check_size(bench_codec.SCHEMA_V4, 1024, schedule, variable=True)
    assert record_codec.decode(bench_codec.SCHEMA_V4, struct.pack("<I", 4) + block,
                               schedule)["samples"] == record_codec.decode(
                                   capture_codec.SCHEMA_V3, block, config)["samples"]
    events = [
        dict(type="session", schema="brickphyr.records/1", session="compact fixture",
             recording=dict(generation=1, size=1024, capacity=4, first=0, next=1,
                            active=False, reason=4, schema=capture_codec.SCHEMA_V3,
                            config_size=len(config), config=config, control=False,
                            variable=True)),
        dict(type="record", index=0, data=block),
        dict(type="recording-end", reason="duration", next=1),
        dict(type="end", session="compact fixture", reason="completed", records=1, gaps=0),
    ]
    saved = Recording(io.BytesIO(b"".join(cbor2.dumps(event) for event in events)))
    observations = list(saved.observations())
    assert saved.complete and len(observations) == len(rows)
    assert [(value.record_index, value.row_index) for value in observations] == [
        (0, index) for index in range(len(rows))]
    assert capture_block_codec.decode(capture_block_codec.encode(iter(rows[:1]), config),
                                      config) == rows[:1]
    with pytest.raises(ValueError, match="Truncated"):
        capture_block_codec.decode(block[:-1], config)
    with pytest.raises(ValueError, match="Extra"):
        capture_block_codec.decode(block + b"\0", config)
    damaged = bytearray(block)
    damaged[6:8] = b"\0\0"  # header's declared row length
    with pytest.raises(ValueError, match="header"):
        capture_block_codec.decode(damaged, config)
    damaged = bytearray(block)
    damaged[8 + len(rows[0]) + 2] |= 0x80  # unused high bit of a 21-field mask
    with pytest.raises(ValueError, match="padding"):
        capture_block_codec.decode(damaged, config)
    damaged = bytearray(block)
    changed_start = 8 + len(rows[0]) + (len(capture_block_codec._field_formats(config)) + 7) // 8
    damaged[changed_start] = 8  # no eight-byte delta is allowed
    with pytest.raises(ValueError, match="delta length"):
        capture_block_codec.decode(damaged, config)
    assert len(block) < sum(map(len, rows))
    with pytest.raises(ValueError, match="row length"):
        capture_block_codec.encode([rows[0], rows[1][:-1]], config)
    with pytest.raises(ValueError, match="1024 bytes"):
        capture_block_codec.encode([rows[0], rows[2]] * 30, config)
    with pytest.raises(ValueError, match="Too many rows"):
        capture_block_codec.encode([rows[0]] * 256, config)


def test_shared_imu_metadata_is_transmitted_once(tmp_path):
    selected = bytearray(15)
    selected[0] = selected[1] = 1
    config = struct.pack("<4I", 3, 15, 30, 1) + selected

    def row(time, sequence, acceleration):
        raw = struct.pack("<2q3Ii", time, time + 2, sequence, 0, 0, 0)
        return (struct.pack("<3qI", time, time + 3, time + 5, 0) +
                raw + struct.pack("<i", acceleration) +
                raw + struct.pack("<IiIi", 1, 0, 1, acceleration))

    rows = [row(100_000, 7, 100), row(115_000, 8, 110), row(130_000, 9, 120)]
    block = capture_block_codec.encode(rows, config)
    assert _native_encode(tmp_path, rows, config) == block
    assert capture_block_codec.decode(block, config) == rows
    assert len(block) < 8 + 3 * len(rows[0]) - 32
    unequal = bytearray(rows[1])
    unequal[28 + 36] ^= 1
    with pytest.raises(ValueError, match="Shared IMU"):
        capture_block_codec.encode([rows[0], unequal], config)
    damaged = bytearray(block)
    alias = capture_block_codec._aliases(config, len(capture_block_codec._field_formats(config)))
    first_alias = alias.index(4)
    damaged[8 + len(rows[0]) - 32 + first_alias // 8] &= ~(1 << (first_alias % 8))
    with pytest.raises(ValueError, match="Shared IMU"):
        capture_block_codec.decode(damaged, config)


def test_control_block_exact_rows_and_corruption(tmp_path):
    selected = bytearray(15)
    selected[0] = selected[1] = 1
    config = struct.pack("<4I", 3, 15, 30, 1) + selected

    def row(time, sequence, acceleration, revision, parameter_size):
        step = struct.pack("<2q4I", time - 10, time, sequence, 0,
                           revision, parameter_size)
        raw = struct.pack("<2q3Ii", time, time + 2, sequence, 0, 0, 0)
        capture = (struct.pack("<3qI", time, time + 3, time + 5, 0) +
                   raw + struct.pack("<i", acceleration) + raw +
                   struct.pack("<IiIi", 1, 0, 1, acceleration))
        return step + capture

    rows = [row(100_000, 7, 100, 3, 16), row(115_000, 8, 110, 3, 16),
            row(130_000, 9, 120, 4, 20)]
    block = _native_encode(tmp_path, rows, config, control=True)
    assert struct.unpack_from("<4sHH", block) == (b"BCB3", len(rows), len(rows[0]))
    assert capture_block_codec.decode_control(block, config) == rows
    compact = _native_encode(tmp_path, rows, config, control=True, compact_first=True)
    assert compact[:4] == b"BCB4" and len(compact) < len(block)
    assert capture_block_codec.decode_control(compact, config) == rows
    assert len(block) < 8 + sum(map(len, rows)) - 32

    with pytest.raises(ValueError, match="Truncated"):
        capture_block_codec.decode_control(block[:-1], config)
    with pytest.raises(ValueError, match="Extra"):
        capture_block_codec.decode_control(block + b"\0", config)
    with pytest.raises(ValueError, match="header"):
        capture_block_codec.decode_control(capture_block_codec.encode(
            [value[32:] for value in rows], config), config)

    aliases = [-1] * 6 + [
        alias + 6 if alias >= 0 else -1
        for alias in capture_block_codec._aliases(
            config, len(capture_block_codec._field_formats(config)))
    ]
    widths = [8, 8, 4, 4, 4, 4] + [
        field.size for field in capture_block_codec._field_formats(config)
    ]
    first_size = sum(width for width, alias in zip(widths, aliases, strict=True)
                     if alias < 0)
    mask_start = 8 + first_size
    damaged = bytearray(block)
    first_alias = next(index for index, alias in enumerate(aliases) if alias >= 0)
    damaged_compact = bytearray(compact)
    damaged_compact[8 + first_alias // 2] |= 1 << (4 * (first_alias % 2))
    with pytest.raises(ValueError, match="Aliased first-row"):
        capture_block_codec.decode_control(damaged_compact, config)
    damaged_compact = bytearray(compact)
    damaged_compact[8 + len(widths) // 2] |= 0x10
    with pytest.raises(ValueError, match="padding nibble"):
        capture_block_codec.decode_control(damaged_compact, config)
    damaged_compact = bytearray(compact)
    damaged_compact[8 + 2 // 2] = (damaged_compact[8 + 2 // 2] & 0xF0) | 15
    with pytest.raises(ValueError, match="field width"):
        capture_block_codec.decode_control(damaged_compact, config)
    with pytest.raises(ValueError, match="Truncated"):
        capture_block_codec.decode_control(compact[:-1], config)
    damaged[mask_start + first_alias // 8] &= ~(1 << (first_alias % 8))
    with pytest.raises(ValueError, match="Shared IMU"):
        capture_block_codec.decode_control(damaged, config)
    damaged = bytearray(block)
    damaged[mask_start + (len(widths) + 7) // 8] = 8
    with pytest.raises(ValueError, match="delta length"):
        capture_block_codec.decode_control(damaged, config)
