# SPDX-License-Identifier: Apache-2.0
"""Retained cycle collection across overwrite and run changes."""

import asyncio
import contextlib
import io
import json
import struct
from types import SimpleNamespace

import pytest

from brickphyrdev import recording


def _info(**changes):
    fields = dict(
        generation=7, size=4, capacity=3, first=0, next=1, active=True,
        record_schema="cycle/1", config_size=0, trigger=None, after=0, reason=0,
    )
    if "active" in changes and "reason" not in changes:
        fields["reason"] = 0 if changes["active"] else 1
    return SimpleNamespace(**(fields | changes))


def _fragment(**changes):
    fields = dict(
        generation=7, first=0, next=1, active=True, data=b"",
        trigger=None, after=0, reason=0,
    )
    if "active" in changes and "reason" not in changes:
        fields["reason"] = 0 if changes["active"] else 1
    return SimpleNamespace(**(fields | changes))


def _fixture(monkeypatch, fragments, info, *, terminal_info=None, infos=None,
             configs=(), triggers=None, status=None):
    reads = []
    infos = iter(infos) if infos is not None else None
    configs = iter(configs)
    first_info = True
    latest = info

    async def checked(_client, request):
        nonlocal first_info, latest
        if isinstance(request, recording.Info):
            if infos is not None:
                return next(infos)
            if first_info:
                first_info = False
                return info
            if terminal_info is not None:
                return terminal_info
            return SimpleNamespace(**(vars(info) | {
                "first": latest.first, "next": latest.next,
                "active": False, "trigger": latest.trigger,
                "after": latest.after, "reason": 1,
            }))
        if isinstance(request, recording.Config):
            reads.append(("config", request.off))
            return next(configs)
        if isinstance(request, recording.Trigger):
            if triggers is not None:
                triggers.append((request.generation, request.after))
            return SimpleNamespace()
        if isinstance(request, recording.application.Status):
            return status or SimpleNamespace(
                model_dump=lambda **_kw: {"api": 1, "abi": "test", "state": 1},
                state=3, result=-1, fault=2, cleanup=-5,
            )
        assert isinstance(request, recording.Record)
        reads.append((request.index, request.off))
        latest = next(fragments)
        return latest

    async def identity(_client):
        return SimpleNamespace(firmware="test", build="fixture", board="virtual")

    monkeypatch.setattr(recording, "checked", checked)
    monkeypatch.setattr(recording.telemetry, "info", identity)
    return reads


def test_recording_discards_partial_on_overwrite_and_reports_gaps(monkeypatch):
    replies = iter(
        (
            _fragment(first=2, next=4, data=b"ab"),
            _fragment(first=3, next=4),
            _fragment(first=3, next=4, active=False, data=b"WXYZ"),
        )
    )
    reads = _fixture(monkeypatch, replies, _info(first=2, next=4))
    output = io.StringIO()
    counts = {"records": 0, "gaps": 0}
    asyncio.run(recording.collect(object(), output, "fixture", counts))
    events = [json.loads(line) for line in output.getvalue().splitlines()]
    assert [(event["from"], event["to"]) for event in events if event["type"] == "gap"] == [
        (0, 2),
        (2, 3),
    ]
    assert [(event["index"], event["data"]) for event in events if event["type"] == "record"] == [
        (3, "5758595a")
    ]
    assert reads == [(2, 0), (2, 2), (3, 0)]
    assert counts == {"records": 1, "gaps": 2}
    assert next(event for event in events if event["type"] == "application-end") == {
        "type": "application-end", "state": 3, "result": -1, "fault": 2, "cleanup": -5,
    }


def test_recording_generation_change_ends_without_mixing_partial(monkeypatch):
    replies = iter((_fragment(data=b"ab"), _fragment(generation=8, data=b"cd")))
    reads = _fixture(monkeypatch, replies, _info())

    @contextlib.asynccontextmanager
    async def connection(**_kw):
        yield object(), object()

    monkeypatch.setattr(recording, "connection", connection)
    output = io.StringIO()
    args = SimpleNamespace(ble=None, port="virtual", serial=None)
    with pytest.raises(RuntimeError, match="generation changed from 7 to 8"):
        asyncio.run(recording._record(args, output))
    events = [json.loads(line) for line in output.getvalue().splitlines()]
    assert reads == [(0, 0), (0, 2)]
    assert not any(event["type"] == "record" for event in events)
    assert events[-1]["type"] == "end"
    assert events[-1]["reason"] == "error"
    assert events[-1]["records"] == 0


def test_recording_decodes_control_input_and_preserves_raw_bytes(monkeypatch):
    raw = struct.pack(
        "<8q10i6I2q4i4I",
        1000, 1012, 900, 950, 11, 12, -1300, 2400,
        -1, 2, -3, 4, -5, 6, -7, -8, -9, -10,
        21, 22, 23, 24, 7450, 5,
        1100, 1200,
        -300, 400, -11, -12,
        31, 32, 33, 34,
    )
    assert len(raw) == 176
    replies = iter(
        (
            _fragment(data=raw[:100]),
            _fragment(active=False, data=raw[100:]),
        )
    )
    _fixture(
        monkeypatch, replies,
        _info(size=176, record_schema="brickphyr.control-input/1"),
    )
    output = io.StringIO()
    asyncio.run(recording.collect(object(), output, "fixture", {"records": 0, "gaps": 0}))
    record = next(
        event for event in map(json.loads, output.getvalue().splitlines())
        if event["type"] == "record"
    )
    assert record["data"] == raw.hex()
    assert record["values"]["capture_end_us"] == 1012
    assert record["values"]["angle_mdeg"] == [-1300, 2400]
    assert record["values"]["acceleration"] == [-1, 2, -3]
    assert record["values"]["errors"] == [-7, -8, -9, -10]
    assert record["values"]["battery_mv"] == 7450
    assert record["values"]["applied_ms"] == [1100, 1200]
    assert record["values"]["duty_permille"] == [-300, 400]
    assert record["values"]["output_generation"] == [33, 34]


def test_recording_rejects_terminal_status_from_new_generation(monkeypatch):
    _fixture(
        monkeypatch, iter(()), _info(next=0, active=False),
        terminal_info=_info(generation=8, next=0, active=False),
    )
    output = io.StringIO()
    with pytest.raises(RuntimeError, match="generation changed from 7 to 8"):
        asyncio.run(
            recording.collect(object(), output, "fixture", {"records": 0, "gaps": 0})
        )
    assert not any(
        event["type"] == "application-end"
        for event in map(json.loads, output.getvalue().splitlines())
    )


def test_recording_config_fragments_and_generation_race(monkeypatch):
    config = bytes(range(256)) + b"tail"
    initial = _info(config_size=len(config))
    responses = (
        SimpleNamespace(generation=7, total=len(config), data=config[:256]),
        SimpleNamespace(generation=7, total=len(config), data=config[256:]),
    )
    reads = _fixture(
        monkeypatch, iter((_fragment(active=False, data=b"ABCD"),)), initial,
        infos=(initial, initial, _info(config_size=len(config), active=False)),
        configs=responses,
    )
    output = io.StringIO()
    asyncio.run(recording.collect(object(), output, "fixture", {"records": 0, "gaps": 0}))
    events = [json.loads(line) for line in output.getvalue().splitlines()]
    assert events[0]["recording"]["config"] == config.hex()
    assert reads[:2] == [("config", 0), ("config", 256)]

    _fixture(
        monkeypatch, iter(()), initial,
        configs=(responses[0], SimpleNamespace(
            generation=8, total=len(config), data=config[256:]
        )),
    )
    output = io.StringIO()
    with pytest.raises(RuntimeError, match="generation changed from 7 to 8"):
        asyncio.run(recording.collect(object(), output, "fixture", {"records": 0, "gaps": 0}))
    assert output.getvalue() == ""


def test_recording_trigger_completion_keeps_live_application(monkeypatch):
    triggered = _info(trigger=1, after=1)
    frozen = _info(next=2, active=False, trigger=1, after=1, reason=2)
    triggers = []
    status = SimpleNamespace(
        model_dump=lambda **_kw: {"state": 1},
        state=1, result=0, fault=0, cleanup=0,
    )
    _fixture(
        monkeypatch,
        iter((
            _fragment(trigger=1, after=1, data=b"ABCD"),
            _fragment(next=2, active=False, trigger=1, after=1, reason=2, data=b"EFGH"),
        )),
        _info(), infos=(_info(), triggered, frozen), triggers=triggers, status=status,
    )
    output = io.StringIO()
    asyncio.run(recording.collect(
        object(), output, "fixture", {"records": 0, "gaps": 0}, after=1
    ))
    events = [json.loads(line) for line in output.getvalue().splitlines()]
    assert triggers == [(7, 1)]
    assert [event["index"] for event in events if event["type"] == "record"] == [0, 1]
    assert events[-1] == {
        "type": "recording-end", "reason": "trigger", "state": 1,
        "result": 0, "fault": 0, "cleanup": 0,
        "capture": {"trigger": 1, "after": 1, "next": 2, "complete": True},
    }
    assert not any(event["type"] == "application-end" for event in events)

    interrupted = _info(active=False, trigger=1, after=1)
    _fixture(
        monkeypatch,
        iter((_fragment(active=False, trigger=1, after=1, data=b"ABCD"),)),
        _info(), infos=(_info(), triggered, interrupted), status=status,
    )
    output = io.StringIO()
    asyncio.run(recording.collect(
        object(), output, "fixture", {"records": 0, "gaps": 0}, after=1
    ))
    events = [json.loads(line) for line in output.getvalue().splitlines()]
    assert events[-1]["type"] == "application-end"
    assert events[-1]["capture"] == {
        "trigger": 1, "after": 1, "next": 1, "complete": False,
    }
