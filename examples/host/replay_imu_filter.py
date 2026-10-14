# SPDX-License-Identifier: Apache-2.0
"""Replay recorded IMU FIR steps with the same CMSIS-DSP C routine as the hub."""

import argparse
import ctypes
import json
import math
import struct
from pathlib import Path

from brickphyrdev.records import Recording

_RECORD = struct.Struct("<q4I10f")
_FOUR_FLOATS = ctypes.c_float * 4


def replay(source, library, *, allow_gaps=False):
    recording = Recording(source)
    if (recording.schema != "brickphyr.imu-fir/1" or recording.size != _RECORD.size or
        len(recording.config) != 16 or recording.control):
        raise ValueError("Expected a brickphyr.imu-fir/1 recording with four float32 coefficients")
    coefficients = struct.unpack("<4f", recording.config)
    if not all(math.isfinite(value) for value in coefficients):
        raise ValueError("Non-finite FIR coefficients")
    coefficients = _FOUR_FLOATS(*coefficients)
    step = library.brickphyr_imu_fir_step
    float_pointer = ctypes.POINTER(ctypes.c_float)
    step.argtypes = [float_pointer, float_pointer, ctypes.c_float, float_pointer, float_pointer]
    step.restype = None
    output = ctypes.c_float()
    after = _FOUR_FLOATS()
    previous = None
    mismatches = 0
    first_mismatch = None
    # Each frame stores its original input state, so a missing interval need not
    # be invented to repeat this individual step. It still prevents full replay.
    for frame in recording.frames(allow_gaps=allow_gaps):
        sample_us, sequence, epoch, reset, reserved, raw, filtered, *history = _RECORD.unpack(frame.data)
        before_bytes, after_bytes = frame.data[32:48], frame.data[48:64]
        if (sample_us < 0 or reset not in (0, 1) or reserved or
            not all(math.isfinite(value) for value in (raw, filtered, *history))):
            raise ValueError(f"Record {frame.index}: invalid FIR observation")
        if frame.index == 0 and not reset:
            raise ValueError("First FIR observation must reset the filter")
        if reset:
            if any(history[:4]):
                raise ValueError(f"Record {frame.index}: reset with nonzero history")
        elif previous is not None and not frame.gap:
            if epoch != previous[0] or before_bytes != previous[1]:
                raise ValueError(f"Record {frame.index}: discontinuous FIR state without reset")
            if sequence == previous[2]:
                raise ValueError(f"Record {frame.index}: duplicate IMU observation without reset")
        step(coefficients, _FOUR_FLOATS(*history[:4]), raw, ctypes.byref(output), after)
        actual = struct.pack("<5f", output.value, *after)
        expected = frame.data[28:32] + after_bytes
        if actual != expected:
            mismatches += 1
            if first_mismatch is None:
                first_mismatch = {"index": frame.index, "expected": expected.hex(),
                                  "computed": actual.hex()}
        previous = epoch, after_bytes, sequence
    if not recording.count:
        raise ValueError("Recording contains no FIR steps")
    return {
        "schema": recording.schema,
        "records": recording.count,
        "missing_records": recording.missing,
        "bit_exact_records": recording.count - mismatches,
        "first_mismatch": first_mismatch,
        "collection_complete": recording.complete,
        "outcome": recording.outcome,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", help="Completed JSONL capture from imu_record.elf")
    parser.add_argument("--library", type=Path, required=True, help="Built desktop FIR shared library")
    parser.add_argument(
        "--allow-gaps", action="store_true",
        help="Replay retained steps from their saved states despite missing history",
    )
    args = parser.parse_args()
    try:
        library = ctypes.CDLL(str(args.library.resolve()))
        with open(args.file, encoding="utf-8") as source:
            result = replay(source, library, allow_gaps=args.allow_gaps)
        print(json.dumps(result, indent=2))
        if result["first_mismatch"] is not None:
            parser.exit(1)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
