# SPDX-License-Identifier: Apache-2.0
"""Numerical replay uses the actual CMSIS host build, never a copied filter."""

import ctypes
import io
import json
import struct
from pathlib import Path

import pytest

from examples.host.replay_imu_filter import replay


def test_fir_replay_checks_result_state_and_missing_history():
    path = Path("build/imu-replay/libbrickphyr_imu_fir.so")
    if not path.exists():
        pytest.skip("Build examples/host into build/imu-replay before this check")
    library = ctypes.CDLL(str(path.resolve()))
    # Two known scalar FIR observations, with a retained tail starting at 3.
    # These golden values check both output and CMSIS's four-word state layout.
    events = [
        {"type": "session", "schema": "brickphyr.records/1", "session": "fir-fixture",
         "recording": dict(generation=1, size=64, capacity=64, first=3, next=5,
                           active=False, reason=1, schema="brickphyr.imu-fir/1", config_size=16,
                           config=struct.pack("<4f", .25, .25, .25, .25).hex())},
        {"type": "gap", "from": 0, "to": 3},
        {"type": "record", "index": 3, "data": struct.pack(
            "<q4I10f", 1000, 1, 1, 1, 0, 4, 1, 0, 0, 0, 0, 0, 0, 4, 4).hex()},
        {"type": "record", "index": 4, "data": struct.pack(
            "<q4I10f", 2000, 2, 1, 0, 0, 8, 3, 0, 0, 4, 4, 0, 4, 8, 8).hex()},
        {"type": "application-end", "state": 2, "result": 0, "fault": 0, "cleanup": 0},
        {"type": "end", "session": "fir-fixture", "reason": "completed", "records": 2, "gaps": 1},
    ]

    def run(*, allow_gaps=True):
        return replay(io.StringIO("".join(json.dumps(event) + "\n" for event in events)),
                      library, allow_gaps=allow_gaps)

    with pytest.raises(ValueError, match="Missing records"):
        run(allow_gaps=False)
    result = run()
    assert result["bit_exact_records"] == 2 and result["missing_records"] == 3
    assert result["collection_complete"] and result["first_mismatch"] is None
    data = bytearray.fromhex(events[3]["data"])
    struct.pack_into("<I", data, 8, 1)
    events[3]["data"] = data.hex()
    with pytest.raises(ValueError, match="duplicate IMU observation"):
        run()
    struct.pack_into("<I", data, 8, 2)
    struct.pack_into("<f", data, 28, 99)  # Corrupt the recorded numerical output.
    events[3]["data"] = data.hex()
    result = run()
    assert result["bit_exact_records"] == 1 and result["first_mismatch"]["index"] == 4
    struct.pack_into("<f", data, 32, 1)  # Break continuity without a reset or gap.
    events[3]["data"] = data.hex()
    with pytest.raises(ValueError, match="discontinuous FIR state"):
        run()
