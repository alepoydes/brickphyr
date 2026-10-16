# SPDX-License-Identifier: Apache-2.0
"""Synthetic wire bytes for the built-in capture decoder."""

import io
import json
import struct

import pytest

from brickphyrdev import capture_codec, record_codec
from brickphyrdev.records import Recording


def test_capture_values_and_offline_reader():
    config = struct.pack("<4I", 0b11111, 20, 1000, 1)
    frame = b"".join((
        struct.pack("<3qI", 100, 101, 102, 1),
        struct.pack("<2q3Ii6i", 10, 11, 2, 3, 4, -5, -6, 7, -8, 9, -10, 11),
        struct.pack("<2q3Ii6iIiI", 10, 11, 2, 3, 4, -5,
                    -12, 13, -14, 15, -16, 17, 8, -9, 3),
        struct.pack("<2q4Ii2I", 1, 2, 7600, 400, 3, 1, -11, 7500, 3),
        struct.pack("<2q2Ii", 44, -12345, 2, 3, -12),
        struct.pack("<2q5I3i4B", 50, 51, 2, 7, 7, 2, 2,
                    -13, -14, -500, 2, 1, 0, 3),
    ))
    record_codec.check_size("brickphyr.capture/1", len(frame), config)
    values = record_codec.decode("brickphyr.capture/1", frame, config)
    assert (values["scheduled_us"], values["skipped"], values["resolution_us"]) == (
        100, 1, 1
    )
    assert values["sources"]["imu"]["acceleration_um_s2"] == [-6, 7, -8]
    assert values["sources"]["imu"]["error"] == -5
    assert values["sources"]["imu.calibrated"]["error"] == -9
    assert values["sources"]["imu.calibrated"]["ready"] is True
    assert values["sources"]["imu.calibrated"]["stationary"] is True
    assert values["sources"]["battery"]["critical"] is True
    assert values["sources"]["encoder.A"]["angle_mdeg"] == -12345
    assert values["sources"]["motor.A"]["duty_permille"] == -500
    assert values["sources"]["motor.A"]["output_error"] == -13

    session = {
        "type": "session", "schema": "brickphyr.records/1", "session": "capture",
        "recording": {
            "generation": 1, "size": len(frame), "capacity": 1, "first": 0, "next": 1,
            "active": True, "reason": 0, "schema": "brickphyr.capture/1",
            "config_size": len(config), "config": config.hex(), "control": False,
        },
    }
    stream = io.StringIO("".join(json.dumps(event) + "\n" for event in (
        session, {"type": "record", "index": 0, "data": frame.hex()},
    )))
    assert list(Recording(stream).frames(allow_incomplete=True))[0].values == values

    for bad in (
        config[:-1], struct.pack("<4I", 0, 20, 1000, 1),
        struct.pack("<4I", 1 << 15, 20, 1000, 1),
        struct.pack("<4I", 1, 0, 1000, 1),
        struct.pack("<4I", 1, 20, 1000, 0),
    ):
        with pytest.raises(RuntimeError, match="configuration"):
            record_codec.check_size("brickphyr.capture/1", len(frame), bad)
    with pytest.raises(RuntimeError, match="record length"):
        record_codec.decode("brickphyr.capture/1", frame[:-1], config)


def test_selected_capture_fields_are_flat_and_omitted_fields_are_absent():
    fields = bytes((0b100001, 0b010000, 0b010, 1, 0b10010)) + bytes(10)
    config = struct.pack("<4I", 0b11111, 20, 1000, 1) + fields
    frame = b"".join((
        struct.pack("<3qI", 100, 101, 102, 1),
        struct.pack("<2q3Ii2i", 10, 11, 2, 3, 4, -5, -6, 7),
        struct.pack("<2q3IiIiIi", 20, 21, 6, 7, 8, -9, 10, -11, 3, -12),
        struct.pack("<2q2Ii2I", 30, 31, 12, 13, -14, 3, 420),
        struct.pack("<qIIiq", 40, 15, 16, -17, -12345),
        struct.pack("<qIIii4BqI", 50, 18, 19, -20, -21, 2, 1, 0, 3, 51, 22),
    ))
    record_codec.check_size(capture_codec.SCHEMA_V2, len(frame), config)
    values = record_codec.decode(capture_codec.SCHEMA_V2, frame, config)
    assert values["sources"]["imu"] == dict(sample_us=10, read_us=11, sequence=2,
                                                missed=3, failures=4, error=-5,
                                                ax_um_s2=-6, gz_mdps=7)
    assert values["sources"]["imu.calibrated"]["flags"] == 3
    assert values["sources"]["imu.calibrated"]["gy_mdps"] == -12
    assert "ax_um_s2" not in values["sources"]["imu.calibrated"]
    assert values["sources"]["battery"]["current_ma"] == 420
    assert "voltage_mv" not in values["sources"]["battery"]
    assert values["sources"]["encoder.A"]["angle_mdeg"] == -12345
    assert values["sources"]["motor.A"]["request_generation"] == 22
    assert "duty_permille" not in values["sources"]["motor.A"]

    session = {
        "type": "session", "schema": "brickphyr.records/1", "session": "selected capture",
        "recording": {
            "generation": 1, "size": len(frame), "capacity": 1, "first": 0, "next": 1,
            "active": True, "reason": 0, "schema": capture_codec.SCHEMA_V2,
            "config_size": len(config), "config": config.hex(), "control": False,
        },
    }
    stream = io.StringIO("".join(json.dumps(event) + "\n" for event in (
        session, {"type": "record", "index": 0, "data": frame.hex()},
    )))
    assert list(Recording(stream).frames(allow_incomplete=True))[0].values == values

    for bad in (config[:-1],
                struct.pack("<4I", 0b11111, 20, 1000, 1) + bytes((0, *fields[1:])),
                struct.pack("<4I", 0b11111, 20, 1000, 1) + bytes((0x40, *fields[1:])),
                config[:-1] + b"\x01",
                struct.pack("<4I", 0b11110, 20, 1000, 1) + fields):
        with pytest.raises(RuntimeError, match="configuration|field mask"):
            record_codec.check_size(capture_codec.SCHEMA_V2, len(frame), bad)
    with pytest.raises(RuntimeError, match="record length"):
        record_codec.decode(capture_codec.SCHEMA_V2, frame + b"\0", config)
