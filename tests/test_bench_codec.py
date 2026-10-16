# SPDX-License-Identifier: Apache-2.0
"""Mixed bench slots retain their meaning through the offline recording reader."""

import io
import struct

import cbor2
import pytest

from brickphyrdev import bench_codec
from brickphyrdev.records import Recording


def _fixture():
    config = b"".join((
        struct.pack("<12I", 1 << 2, 10, 100, 1, 3, 2, 7, 8, 0, 0, 0, 0),
        struct.pack("<3I6h", 0, 60, 0, 250, -300, 0, 0, 0, 0),
        struct.pack("<3I6h", 70, 0, 3, 0, 0, 0, 0, 0, 0),
    ))
    size = 56 + 2 * 40

    def slot(data):
        assert len(data) <= size
        return data.ljust(size, b"\0")

    sample = slot(b"".join((
        struct.pack("<I3qI", 0, 100_000, 100_010, 100_020, 0),
        struct.pack("<2q4Ii2I", 10, 11, 7600, 100, 1, 0, -5, 7500, 1),
    )))
    group = slot(b"".join((
        struct.pack("<IIqqIiii4I", 1, 1, 100_000, 100_030, 42, 0, 0, 0, 2, 3, 0, 1),
        struct.pack("<IIiIqqii", 7, 11, 250, 2, 100_040, 0, 0, 0),
        struct.pack("<IIiIqqii", 8, 12, -300, 2, 100_040, 0, 0, 0),
    )))
    data = [slot(struct.pack("<Iq", 2, 100_000)), sample, group,
            slot(struct.pack("<Iqii4I", 3, 200_000, 0, 0, 0, 10, 1, 1))]
    session = {
        "type": "session", "schema": "brickphyr.records/1", "session": "bench fixture",
        "recording": {
            "generation": 1, "size": size, "capacity": 4, "first": 0, "next": 4,
            "active": False, "reason": 3, "schema": bench_codec.SCHEMA,
            "config_size": len(config), "config": config, "control": False,
        },
    }
    events = [session, *(dict(type="record", index=i, data=value)
                         for i, value in enumerate(data)),
              dict(type="recording-end", reason="full", next=4),
              dict(type="end", session="bench fixture", reason="completed", records=4, gaps=0)]
    return config, data, events


def test_bench_mixed_slots_and_truncated_cbor_event():
    config, data, events = _fixture()
    stream = io.BytesIO(b"".join(cbor2.dumps(event) for event in events))
    recording = Recording(stream)
    frames = list(recording.frames())
    assert recording.complete and recording.gaps == 0
    assert [frame.values["kind"] for frame in frames] == ["begin", "sample", "group", "end"]
    assert frames[0].values["origin_us"] == 100_000
    assert frames[1].values["sources"]["battery"]["error"] == -5
    assert frames[1].values["scheduled_us"] == 100_000
    assert frames[2].values["action"] == 1
    assert frames[2].values["ports"]["B"]["duty_permille"] == -300
    assert frames[2].values["active"] is True
    assert frames[3].values["attempted_actions"] == 1

    truncated = b"".join(cbor2.dumps(event) for event in events[:3]) + cbor2.dumps(events[3])[:-1]
    with pytest.raises(ValueError, match="Truncated CBOR event"):
        list(Recording(io.BytesIO(truncated)).frames(allow_incomplete=True))

    with pytest.raises(RuntimeError, match="configuration length"):
        bench_codec.check_size(len(data[0]), config[:-1])
    with pytest.raises(RuntimeError, match="port mask"):
        bench_codec.check_size(len(data[0]), config[:16] + struct.pack("<I", 0) + config[20:])
    with pytest.raises(RuntimeError, match="record length"):
        bench_codec.decode(data[1][:-1], config)
    with pytest.raises(RuntimeError, match="slot kind"):
        bench_codec.decode(struct.pack("<I", 4) + data[0][4:], config)
    with pytest.raises(RuntimeError, match="slot padding"):
        bench_codec.decode(data[1][:-1] + b"\x01", config)


def test_compact_bench_records_keep_exact_kind_lengths():
    config, padded, events = _fixture()
    events[0]["recording"].update(schema=bench_codec.SCHEMA_V2, variable=True)
    sizes = (12, 76, 136, 36)
    for event, data, size in zip(events[1:5], padded, sizes, strict=True):
        event["data"] = data[:size]

    stream = io.BytesIO(b"".join(cbor2.dumps(event) for event in events))
    saved = Recording(stream)
    frames = list(saved.frames())
    assert saved.complete and saved.variable
    assert [frame.values["kind"] for frame in frames] == ["begin", "sample", "group", "end"]
    assert [len(frame.data) for frame in frames] == list(sizes)
    assert frames[1].values["sources"]["battery"]["error"] == -5
    assert frames[2].values["ports"]["B"]["duty_permille"] == -300

    events[1]["data"] += b"\0"  # Padding is forbidden even below the maximum record size.
    with pytest.raises(ValueError, match="brickphyr.bench/2 record length"):
        list(Recording(io.BytesIO(b"".join(cbor2.dumps(event) for event in events))).frames())

    events[1]["data"] = b""
    with pytest.raises(ValueError, match="record length"):
        list(Recording(io.BytesIO(b"".join(cbor2.dumps(event) for event in events))).frames())
