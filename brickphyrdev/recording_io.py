# SPDX-License-Identifier: Apache-2.0
"""Write application recording events as JSONL or a CBOR Sequence."""

import io

import cbor2

from . import monitor


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
    if log is None:
        return
    if isinstance(log, io.TextIOBase):
        monitor.write_log(log, _json_value(event))
    elif isinstance(log, (io.BufferedIOBase, io.RawIOBase)):
        cbor2.dump(event, log)
        log.flush()
    else:
        raise TypeError("Recording stream must be text or binary IO")
