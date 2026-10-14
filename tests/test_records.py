# SPDX-License-Identifier: Apache-2.0
"""Offline replay must not hide lost data, parameters or collection errors."""

import copy
import io
import json

import pytest

from brickphyrdev.records import Recording, run


def _events():
    step = dict(scheduled_us=100, started_us=110, sequence=1, skipped=0,
                parameter_revision=2, parameter_size=1)
    return [
        {"type": "session", "schema": "brickphyr.records/1", "session": "fixture",
         "recording": dict(generation=7, size=1, capacity=3, first=2, next=4,
                           active=False, reason=1, schema="app/1", config_size=1, config="ff",
                           control=True, parameter_size=1)},
        {"type": "gap", "from": 0, "to": 2},
        {"type": "parameters", "revision": 2, "data": "12"},
        {"type": "record", "index": 2, "data": "34", "control": step},
        {"type": "parameters", "revision": 3, "data": "56"},
        {"type": "record", "index": 3, "data": "78", "control": step | {
            "scheduled_us": 200, "started_us": 210, "sequence": 2, "parameter_revision": 3,
        }},
        {"type": "application-end", "state": 2, "result": 0, "fault": 0, "cleanup": 0},
        {"type": "end", "session": "fixture", "reason": "completed", "records": 2, "gaps": 1},
    ]


def _source(events):
    return io.StringIO("".join(json.dumps(event) + "\n" for event in events))


def test_reader_streams_exact_parameters_and_reports_missing_history():
    with pytest.raises(ValueError, match=r"<recording>:2: Missing records \[0, 2\)"):
        list(Recording(_source(_events())).frames())
    recording = Recording(iter(_source(_events())))  # No seeking or whole-file read.
    frames = recording.frames(allow_gaps=True)
    assert recording.config == b"\xff"
    first = next(frames)
    assert (first.index, first.gap, first.data, first.parameters) == (2, 2, b"4", b"\x12")
    assert not recording.complete and recording.outcome is None
    first.control["sequence"] = 999  # Caller mutation cannot affect validation.
    last = list(frames)[0]
    assert (last.index, last.gap, last.data, last.parameters) == (3, 0, b"x", b"V")
    assert recording.complete and recording.count == recording.missing == recording.revisions == 2
    with pytest.raises(ValueError, match="only be consumed once"):
        list(recording.frames())


@pytest.mark.parametrize("event,field,value,message", [
    (3, "index", 3, "Unexpected record index"),
    (4, "revision", 2, "parameter revision"),
    (4, "data", "", "parameters length"),
    (5, "control", dict(scheduled_us=200, started_us=210, sequence=2, skipped=0,
                        parameter_revision=2, parameter_size=1), "invalid times or parameters"),
    (5, "control", dict(scheduled_us=100, started_us=110, sequence=1, skipped=0,
                        parameter_revision=3, parameter_size=1), "moved backwards or changed"),
    (7, "session", "another run", "another session"),
    (7, "records", 1, "counts differ"),
])
def test_reader_rejects_corruption_even_when_allowing_incomplete(event, field, value, message):
    events = _events()
    events[event][field] = value
    with pytest.raises(ValueError, match=message):
        list(Recording(_source(events)).frames(allow_gaps=True, allow_incomplete=True))


def test_incomplete_collection_never_reports_completion():
    metadata = _events()
    del metadata[0]["recording"]["reason"]
    with pytest.raises(ValueError, match="Incomplete recording capture metadata"):
        Recording(_source(metadata))
    for reason in (None, "cancelled", "error"):
        events = _events()
        if reason is None:
            events.pop()
        else:
            events[-1]["reason"] = reason
        with pytest.raises(ValueError, match="Incomplete collection"):
            list(Recording(_source(events)).frames(allow_gaps=True))
        recording = Recording(_source(events))
        assert len(list(recording.frames(allow_gaps=True, allow_incomplete=True))) == 2
        assert not recording.complete
    events = _events()
    del events[-2]
    with pytest.raises(ValueError, match="no application outcome"):
        list(Recording(_source(events)).frames(allow_gaps=True, allow_incomplete=True))
    source = _source(_events()).getvalue() + '{"type":'
    with pytest.raises(ValueError, match=r"<recording>:9:"):
        list(Recording(io.StringIO(source)).frames(allow_gaps=True, allow_incomplete=True))


def test_trigger_capture_reports_running_application_and_rejects_false_completion():
    events = _events()
    events[0]["recording"].update(trigger=3, after=1, reason=2)
    events[-2].update(type="recording-end", reason="trigger", state=1,
                      capture=dict(trigger=3, after=1, next=4, complete=True))
    recording = Recording(_source(events))
    list(recording.frames(allow_gaps=True))
    assert recording.complete and recording.outcome["state"] == 1
    bad = copy.deepcopy(events)
    bad[-2]["capture"]["after"] = 2
    with pytest.raises(ValueError, match="capture boundary"):
        list(Recording(_source(bad)).frames(allow_gaps=True))
    # Stopping before the requested boundary is a completed collection of an
    # incomplete capture, not a failed collector or a completed trigger.
    events[0]["recording"].update(after=2, reason=1)
    events[-2].update(type="application-end", state=2,
                      capture=dict(trigger=3, after=2, next=4, complete=False))
    recording = Recording(_source(events))
    list(recording.frames(allow_gaps=True))
    assert recording.complete and not recording.outcome["capture"]["complete"]


def test_decoder_uses_raw_bytes_not_cached_values():
    events = _events()
    events[0]["recording"].update(schema="brickphyr.control-input/1", size=176,
                                   control=False, parameter_size=0)
    events = [event for event in events if event["type"] != "parameters"]
    for event in events:
        if event["type"] == "record":
            event.pop("control")
            event.update(data=bytes(176).hex(), values={"battery_mv": 12345})
    frames = list(Recording(_source(events)).frames(allow_gaps=True))
    assert all(frame.values["battery_mv"] == 0 for frame in frames)


def test_offline_summary_separates_collection_and_application_result(tmp_path, capsys):
    from types import SimpleNamespace

    events = _events()
    events[-2].update(state=3, result=-1, fault=2)
    path = tmp_path / "records.jsonl"
    path.write_text(_source(events).getvalue())
    run(SimpleNamespace(file=path, allow_incomplete=False))
    result = json.loads(capsys.readouterr().out)
    assert result["collection_complete"] and result["missing_records"] == 2
    assert result["outcome"]["fault"] == 2
