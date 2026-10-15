# SPDX-License-Identifier: Apache-2.0
"""Write application recording events as JSONL or a CBOR Sequence."""

import io
import json

import cbor2


def _json_value(value):
    if isinstance(value, (bytes, bytearray)):
        return value.hex()
    if isinstance(value, dict):
        return {key: _json_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_value(item) for item in value]
    return value


def write_event(log, event):
    """Write one event map to a caller-owned text or binary stream."""
    write_events(log, (event,))


def write_events(log, events):
    """Encode one bounded transfer batch, then write and flush it together."""
    if log is None:
        return
    if isinstance(log, io.TextIOBase):
        data = "".join(json.dumps(_json_value(event), ensure_ascii=True,
                                  separators=(",", ":")) + "\n" for event in events)
    elif isinstance(log, (io.BufferedIOBase, io.RawIOBase)):
        data = b"".join(cbor2.dumps(event) for event in events)
    else:
        raise TypeError("Recording stream must be text or binary IO")
    if data:
        if log.write(data) != len(data):
            raise OSError("Incomplete recording file write")
        log.flush()
