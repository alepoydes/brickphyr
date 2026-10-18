# SPDX-License-Identifier: Apache-2.0
"""Streaming, offline access to application recordings; no hub connection."""

import json
from dataclasses import dataclass
from pathlib import Path

import cbor2

from . import record_codec


def _integer(value, name, minimum=0, maximum=(1 << 64) - 1):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"Invalid {name}: {value!r}")
    return value


def _blob(value, size, name, *, minimum=None):
    if isinstance(value, (bytes, bytearray)):
        data = bytes(value)
    elif isinstance(value, str):
        data = bytes.fromhex(value)
    else:
        raise ValueError(f"Invalid {name} data")
    if minimum is not None and not minimum <= len(data) <= size:
        raise ValueError(f"Invalid {name} length: expected {minimum}..{size}, got {len(data)}")
    if minimum is None and len(data) != size:
        raise ValueError(f"Invalid {name} length: expected {size}, got {len(data)}")
    return data


@dataclass(frozen=True)
class Frame:
    index: int
    data: bytes
    parameters: bytes | None
    control: dict | None
    gap: int
    values: dict | None
    preceding_step_gap: int = 0


@dataclass(frozen=True)
class Observation:
    record_index: int
    row_index: int
    preceding_record_gap: int
    values: dict
    preceding_step_gap: int = 0


class _CountedReader:
    def __init__(self, source):
        self.source = source
        self.count = 0

    def read(self, size=-1):
        data = self.source.read(size)
        self.count += len(data)
        return data

    def readable(self):
        return True

    def seekable(self):
        return False


class Recording:
    """Read one JSONL or CBOR Sequence session from a caller-owned stream.

    Consume frames() to exhaustion before accepting a replay: termination and
    final counts are checked at the end. Gaps and incomplete collection require
    separate, explicit opt-ins. Malformed data is always rejected.
    """

    def __init__(self, source):
        probe = source.read(0) if hasattr(source, "read") else ""
        self._binary = isinstance(probe, bytes)
        self._lines = None if self._binary else iter(source)
        self._reader = _CountedReader(source) if self._binary else None
        self._decoder = cbor2.CBORDecoder(self._reader) if self._binary else None
        self._name = getattr(source, "name", "<recording>")
        self._line = 0
        self._used = False
        self.count = self.gaps = self.missing = self.revisions = 0
        self.step_gaps = self.unrecorded_steps = 0
        self.end = self.outcome = None
        self.complete = False
        self.needs_separator = False
        self.next_index = self.pending_gap = 0
        self.parameter_revision = self.parameters = self.last_control = None
        try:
            self.session = self._read()
            if not self.session or self.session.get("type") != "session" or (
                self.session.get("schema") != "brickphyr.records/1"
            ):
                raise ValueError("Expected a brickphyr.records/1 session")
            self._session_id = self.session["session"]
            if not isinstance(self._session_id, str) or not self._session_id:
                raise ValueError("Missing session identity")
            meta = self.session["recording"]
            self.tag = None
            if meta.get("tag") is not None:
                tag = meta["tag"]
                if isinstance(tag, str) and len(tag) != 32:
                    raise ValueError("Invalid capture tag length")
                self.tag = _blob(tag, 16, "capture tag")
                if not any(self.tag):
                    raise ValueError("Invalid zero capture tag")
            self.schema = meta["schema"]
            if not isinstance(self.schema, str) or not 1 <= len(self.schema) <= 47 or any(
                not 32 <= ord(char) <= 126 for char in self.schema
            ):
                raise ValueError("Invalid application record schema")
            _integer(meta["generation"], "generation", 1, (1 << 32) - 1)
            self.size = _integer(meta["size"], "record size", 1, 1024)
            capacity = _integer(meta["capacity"], "capacity", 1, 32768)
            config_size = _integer(meta.get("config_size", 0), "config size", 0, 1024)
            self.config = _blob(meta.get("config", ""), config_size, "configuration")
            self.control = meta.get("control", False)
            self.variable = meta.get("variable", False)
            self.paused = meta.get("paused", False)
            self.segments = meta.get("segments", False)
            self.parameter_size = _integer(meta.get("parameter_size", 0),
                                           "parameter size", 0, 256)
            if type(self.control) is not bool or not self.control and self.parameter_size:
                raise ValueError("Invalid recording control mode")
            if type(self.variable) is not bool or self.variable and self.control:
                raise ValueError("Invalid variable recording mode")
            if type(self.paused) is not bool or type(self.segments) is not bool or (
                self.segments and (self.paused or self.variable)
            ):
                raise ValueError("Invalid recording delivery mode")
            # Compact records need at least one payload byte, one 16-bit index
            # per slot plus the sentinel, and one 32-bit block pointer.
            if self.variable and self.paused:
                minimum = self.size * capacity + config_size + 2 * capacity
            elif self.variable:
                minimum = config_size + capacity + 2 * (capacity + 1) + 4
            else:
                minimum = self.size * capacity + config_size
            if minimum > 32768:
                raise ValueError("Recording exceeds buffer limit")
            self._first = _integer(meta["first"], "first index")
            self._next = _integer(meta["next"], "next index", self._first)
            if self._next - self._first > capacity:
                raise ValueError("Retained range exceeds capacity")
            self._active = meta["active"]
            if type(self._active) is not bool:
                raise ValueError("Invalid recording active flag")
            self._capacity = capacity
            self._trigger = meta.get("trigger")
            self._after = meta.get("after", 0)
            self._reason = meta.get("reason")
            if self._reason is None:
                if self._trigger is not None or self._after != 0 or config_size:
                    raise ValueError("Incomplete recording capture metadata")
                self._reason = 0 if self._active else 1
            if type(self._reason) is not int or self._reason not in (0, 1, *record_codec.END_REASONS) or (
                self._active != (self._reason == 0)
            ):
                raise ValueError("Invalid recording end reason")
            if (self.paused or self.segments) and (self._trigger is not None or self._reason >= 2):
                raise ValueError("Invalid streaming recording boundary")
            self._capture(self._trigger, self._after, self._next, self._reason == 2)
            if self._reason >= 3:
                self._finite_boundary(self._reason, self._next)
            record_codec.check_size(self.schema, self.size, self.config, variable=self.variable)
        except (KeyError, TypeError, ValueError, RuntimeError) as error:
            raise self._error(error) from error

    def _error(self, error):
        return ValueError(f"{self._name}:{self._line}: {error}")

    def _read(self):
        if self._binary:
            before = self._reader.count
            self._line += 1
            try:
                event = self._decoder.decode()
            except cbor2.CBORDecodeEOF as error:
                if self._reader.count == before:
                    self._line -= 1
                    return None
                raise ValueError("Truncated CBOR event") from error
            except cbor2.CBORDecodeError as error:
                raise ValueError(f"Invalid CBOR event: {error}") from error
            if not isinstance(event, dict):
                raise ValueError("Expected a CBOR event map")
            return event
        line = next(self._lines, None)
        if line is None:
            return None
        self.needs_separator = not line.endswith("\n")
        self._line += 1
        event = json.loads(line)
        if not isinstance(event, dict):
            raise ValueError("Expected a JSON event object")
        return event

    def _capture(self, trigger, after, next_index, complete):
        _integer(after, "post-trigger count", 0, self._capacity - 1)
        if trigger is None:
            if after or complete:
                raise ValueError("Missing capture trigger")
        else:
            _integer(trigger, "trigger", 1, next_index)
            target = _integer(trigger + after, "capture end")
            if next_index > target or complete != (next_index == target):
                raise ValueError("Inconsistent capture boundary")

    def _finite_boundary(self, reason, next_index):
        if (self._trigger is not None or self._first or next_index > self._capacity or
            reason == 3 and not self.variable and next_index != self._capacity):
            raise ValueError("Invalid finite recording boundary")

    def _outcome(self, event, next_index):
        if (self.paused or self.segments) and (event["type"] == "recording-end" or
                                               event.get("capture") is not None):
            raise ValueError("Invalid paused recording boundary")
        reason = event.get("reason")
        finite_reasons = {name: value for value, name in record_codec.END_REASONS.items() if value >= 3}
        if event["type"] == "recording-end" and reason in finite_reasons:
            if self._reason not in (0, finite_reasons[reason]):
                raise ValueError("End event differs from recording end reason")
            if event.get("capture") is not None:
                raise ValueError("Invalid finite recording boundary")
            self._finite_boundary(finite_reasons[reason], next_index)
            if (_integer(event["next"], "capture next index") != next_index or
                next_index < self._next or not self._active and next_index != self._next):
                raise ValueError("Final index differs from recording metadata")
            if reason == "duration":
                if self.variable:
                    if "missed" in event:
                        raise ValueError("Variable recording has no inferred missed releases")
                elif _integer(event["missed"], "missed releases") != self._capacity - next_index:
                    raise ValueError("Missed releases differ from recording metadata")
            return
        for field in ("state", "result", "fault", "cleanup"):
            _integer(event[field], field, -(1 << 31), (1 << 32) - 1)
        if next_index < self._next or not self._active and next_index != self._next:
            raise ValueError("Final index differs from recording metadata")
        triggered = event["type"] == "recording-end"
        if self._reason != 0 and triggered != (self._reason == 2):
            raise ValueError("End event differs from recording end reason")
        capture = event.get("capture")
        if capture is not None:
            if type(capture["complete"]) is not bool or capture["complete"] != triggered:
                raise ValueError("Invalid capture completion flag")
            if _integer(capture["next"], "capture next index") != next_index:
                raise ValueError("Capture end differs from collected range")
            self._capture(capture["trigger"], capture["after"], next_index, triggered)
            if capture["trigger"] is None or self._trigger is not None and (
                (capture["trigger"], capture["after"]) != (self._trigger, self._after)
            ):
                raise ValueError("Capture trigger changed within session")
        elif self._trigger is not None or triggered:
            raise ValueError("Missing capture outcome")
        if triggered and event.get("reason") != "trigger":
            raise ValueError("Unknown recording end reason")

    def frames(self, *, allow_gaps=False, allow_incomplete=False):
        if self._used:
            raise ValueError("Recording frames can only be consumed once")
        self._used = True
        index = gap = 0
        revision = parameters = previous_step = None
        try:
            while (event := self._read()) is not None:
                kind = event.get("type")
                if kind == "resume":
                    if self.outcome is not None or (
                        self.end is not None and self.end["reason"] == "completed"
                    ):
                        raise ValueError("Cannot resume terminated recording")
                    if event["session"] != self._session_id:
                        raise ValueError("Resume event belongs to another session")
                    if _integer(event["from"], "resume index") != index:
                        raise ValueError("Resume index differs from collected range")
                    if not isinstance(event["resumed_utc"], str) or not event["resumed_utc"]:
                        raise ValueError("Invalid resume timestamp")
                    self.end = None
                    continue
                if self.end is not None or self.outcome is not None and kind != "end":
                    raise ValueError("Event after recording termination")
                if kind == "gap":
                    if _integer(event["from"], "gap start") != index:
                        raise ValueError("Gap does not start at the expected index")
                    next_index = _integer(event["to"], "gap end", index + 1)
                    if not self._active and next_index > self._next:
                        raise ValueError("Gap exceeds frozen recording range")
                    if not allow_gaps:
                        raise ValueError(f"Missing records [{index}, {next_index})")
                    gap += next_index - index
                    self.missing += next_index - index
                    self.gaps += 1
                    index = next_index
                    self.next_index = index
                    self.pending_gap = gap
                elif kind == "parameters":
                    if not self.control:
                        raise ValueError("Parameters on an ordinary recording")
                    revision = _integer(event["revision"], "parameter revision",
                                        0 if revision is None else revision + 1,
                                        (1 << 32) - 1)
                    parameters = _blob(event["data"], self.parameter_size, "parameters")
                    self.revisions += 1
                    self.parameter_revision = revision
                    self.parameters = parameters
                elif kind == "record":
                    if _integer(event["index"], "record index", self._first,
                                (1 << 64) - 2) != index:
                        raise ValueError("Unexpected record index; gap or duplicate")
                    if not self._active and index >= self._next:
                        raise ValueError("Record exceeds frozen recording range")
                    data = _blob(event["data"], self.size, "record",
                                 minimum=1 if self.variable else None)
                    control = event.get("control")
                    step_gap = 0
                    if self.control:
                        if not isinstance(control, dict):
                            raise ValueError("Missing control step")
                        step = tuple(_integer(control[field], field, minimum, maximum)
                                     for field, minimum, maximum in (
                                         ("scheduled_us", 0, (1 << 63) - 1),
                                         ("started_us", 0, (1 << 63) - 1),
                                         ("sequence", 1, (1 << 32) - 1),
                                         ("skipped", 0, (1 << 32) - 1),
                                         ("parameter_revision", 0, (1 << 32) - 1),
                                         ("parameter_size", 0, 256),
                                     ))
                        if step[1] < step[0] or step[4:] != (revision, self.parameter_size):
                            raise ValueError("Control step has invalid times or parameters")
                        if previous_step is not None and (
                            any(step[i] < previous_step[i] for i in (0, 1, 2, 4)) or
                            step[2] == previous_step[2] and step != previous_step
                        ):
                            raise ValueError("Control step moved backwards or changed")
                        if previous_step is not None and not gap:
                            step_gap = max(0, step[2] - previous_step[2] - 1)
                            if step_gap:
                                self.step_gaps += 1
                                self.unrecorded_steps += step_gap
                                if not allow_gaps:
                                    raise ValueError(f"Unrecorded control steps before index {index}")
                        previous_step = step
                    elif control is not None:
                        raise ValueError("Control step on an ordinary recording")
                    self.count += 1
                    index += 1
                    self.next_index = index
                    self.last_control = control.copy() if control is not None else None
                    frame = Frame(index - 1, data, parameters, control, gap,
                                  record_codec.decode(self.schema, data, self.config), step_gap)
                    gap = 0
                    self.pending_gap = 0
                    yield frame
                elif kind in ("application-end", "recording-end"):
                    self._outcome(event, index)
                    self.outcome = event
                elif kind == "end":
                    if event["session"] != self._session_id:
                        raise ValueError("End event belongs to another session")
                    if (_integer(event["records"], "record count") != self.count or
                        _integer(event["gaps"], "gap count") != self.gaps):
                        raise ValueError("End counts differ from collected events")
                    reason = event["reason"]
                    if reason not in ("completed", "cancelled", "error"):
                        raise ValueError("Unknown collection end reason")
                    if reason == "completed" and self.outcome is None:
                        raise ValueError("Completed collection has no application outcome")
                    self.end = event
                else:
                    raise ValueError(f"Unknown recording event: {kind!r}")
            if self.end is None or self.end["reason"] != "completed":
                if not allow_incomplete:
                    raise ValueError("Incomplete collection")
            else:
                self.complete = True
        except (KeyError, TypeError, ValueError, RuntimeError) as error:
            raise self._error(error) from error

    def observations(self, *, allow_gaps=False, allow_incomplete=False):
        """Yield individual decoded samples from raw or compact capture records."""
        for frame in self.frames(allow_gaps=allow_gaps, allow_incomplete=allow_incomplete):
            for row_index, values in enumerate(record_codec.samples(frame.values)):
                yield Observation(frame.index, row_index,
                                  frame.gap if row_index == 0 else 0, values,
                                  frame.preceding_step_gap if row_index == 0 else 0)


def run(args):
    format = getattr(args, "format", None)
    binary = format == "cbor" if format else Path(args.file).suffix.lower() == ".cbor"
    source = open(args.file, "rb") if binary else open(args.file, encoding="utf-8")
    with source:
        recording = Recording(source)
        sample_count = payload_bytes = 0
        for frame in recording.frames(allow_gaps=True, allow_incomplete=args.allow_incomplete):
            sample_count += len(record_codec.samples(frame.values))
            payload_bytes += len(frame.data)
        print(json.dumps({
            "session": recording.session["session"],
            **{field: recording.session.get(field) for field in ("board", "firmware", "build")},
            "schema": recording.schema,
            "config_size": len(recording.config),
            "records": recording.count,
            "samples": sample_count,
            "payload_bytes": payload_bytes,
            "gaps": recording.gaps,
            "missing_records": recording.missing,
            "step_gaps": recording.step_gaps,
            "unrecorded_steps": recording.unrecorded_steps,
            "parameter_revisions": recording.revisions,
            "collection_complete": recording.complete,
            "outcome": recording.outcome,
            "end": recording.end,
        }, indent=2))
