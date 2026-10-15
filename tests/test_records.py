# SPDX-License-Identifier: Apache-2.0
"""Offline replay must not hide lost data, parameters or collection errors."""

import copy
import io
import json

import cbor2
import pytest

from brickphyrdev.recording_io import write_event
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


def _binary_events():
    events = copy.deepcopy(_events())
    events[0]["recording"]["config"] = bytearray(b"\xff")
    for event in events:
        if event["type"] in ("parameters", "record"):
            event["data"] = bytes.fromhex(event["data"])
    return events


def test_cbor_sequence_and_jsonl_replay_equivalent(tmp_path, capsys):
    events = _binary_events()
    text, binary = io.StringIO(), io.BytesIO()
    for event in events:
        write_event(text, event)
        write_event(binary, event)
    assert [json.loads(line) for line in text.getvalue().splitlines()] == _events()
    assert cbor2.load(io.BytesIO(binary.getvalue()))["recording"]["config"] == b"\xff"
    json_recording = Recording(io.StringIO(text.getvalue()))
    cbor_recording = Recording(io.BytesIO(binary.getvalue()))
    assert list(json_recording.frames(allow_gaps=True)) == list(
        cbor_recording.frames(allow_gaps=True)
    )
    assert cbor_recording.complete and cbor_recording.config == b"\xff"
    path = tmp_path / "recording.cbor"
    path.write_bytes(binary.getvalue())
    run(type("Args", (), {"file": path, "allow_incomplete": False})())
    assert json.loads(capsys.readouterr().out)["records"] == 2


def test_cbor_truncation_is_not_incomplete_collection():
    events = _binary_events()
    prefix = b"".join(cbor2.dumps(event) for event in events[:3])
    incomplete = cbor2.dumps(events[3])[:-1]
    with pytest.raises(ValueError, match="Truncated CBOR event"):
        list(
            Recording(io.BytesIO(prefix + incomplete)).frames(
                allow_gaps=True, allow_incomplete=True
            )
        )
    recording = Recording(io.BytesIO(prefix))
    assert list(recording.frames(allow_gaps=True, allow_incomplete=True)) == []
    assert not recording.complete


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


def test_resume_preserves_progress_parameters_and_control_order():
    events = _events()
    events[0]["recording"]["tag"] = "01" * 16
    failed_end = {"type": "end", "session": "fixture", "reason": "error",
                  "records": 1, "gaps": 1}
    resume = {"type": "resume", "session": "fixture", "from": 3,
              "resumed_utc": "2026-10-16T12:00:00Z"}
    events[5:5] = [failed_end, resume]
    recording = Recording(_source(events))
    assert recording.tag == b"\x01" * 16
    nullable = _events()
    nullable[0]["recording"]["tag"] = None
    assert Recording(_source(nullable)).tag is None
    binary = _binary_events()
    binary[0]["recording"]["tag"] = b"\x01" * 16
    assert Recording(io.BytesIO(cbor2.dumps(binary[0]))).tag == b"\x01" * 16
    assert (recording.next_index, recording.parameter_revision,
            recording.parameters, recording.last_control) == (0, None, None, None)
    frames = recording.frames(allow_gaps=True)
    first = next(frames)
    assert recording.next_index == 3
    assert recording.parameter_revision == 2 and recording.parameters == b"\x12"
    first.control["sequence"] = 99
    assert recording.last_control["sequence"] == 1
    assert [frame.index for frame in frames] == [3]
    assert recording.complete and recording.end["reason"] == "completed"
    assert (recording.count, recording.gaps, recording.revisions, recording.next_index) == (
        2, 1, 2, 4
    )
    assert recording.parameter_revision == 3 and recording.parameters == b"\x56"
    assert recording.last_control["sequence"] == 2

    unended = copy.deepcopy(events)
    del unended[5]
    without_end = Recording(_source(unended))
    assert len(list(without_end.frames(allow_gaps=True))) == 2
    assert without_end.complete and without_end.next_index == 4

    # The latest parameter event is resume state even without a following frame.
    prefix = Recording(_source(events[:5]))
    list(prefix.frames(allow_gaps=True, allow_incomplete=True))
    assert prefix.tag == b"\x01" * 16
    assert (prefix.next_index, prefix.parameter_revision, prefix.parameters) == (3, 3, b"\x56")

    bad = copy.deepcopy(events)
    bad[7]["control"]["sequence"] = 1
    with pytest.raises(ValueError, match="moved backwards or changed"):
        list(Recording(_source(bad)).frames(allow_gaps=True))


@pytest.mark.parametrize("change,message", [
    ({"session": "other"}, "another session"),
    ({"from": 2}, "Resume index differs"),
    ({"from": True}, "Invalid resume index"),
])
def test_resume_rejects_wrong_identity_or_position(change, message):
    events = _events()
    events.insert(4, {"type": "resume", "session": "fixture", "from": 3,
                      "resumed_utc": "2026-10-16T12:00:00Z"} | change)
    with pytest.raises(ValueError, match=message):
        list(Recording(_source(events)).frames(allow_gaps=True))


@pytest.mark.parametrize("cut", [7, 8])
def test_resume_rejects_application_outcome_and_completed_end(cut):
    events = _events()
    events.insert(cut, {"type": "resume", "session": "fixture", "from": 4,
                        "resumed_utc": "2026-10-16T12:00:00Z"})
    with pytest.raises(ValueError, match="Cannot resume terminated recording"):
        list(Recording(_source(events)).frames(allow_gaps=True))


@pytest.mark.parametrize("tag", ["00" * 16, "01" * 15, b"\x01" * 15])
def test_reader_rejects_invalid_capture_tag(tag):
    events = _events()
    events[0]["recording"]["tag"] = tag
    source = io.BytesIO(b"".join(cbor2.dumps(event) for event in events))
    with pytest.raises(ValueError, match="capture tag"):
        Recording(source)


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
