# SPDX-License-Identifier: Apache-2.0
"""Exercise the production recorder against the native_sim system heap."""

import subprocess
from pathlib import Path


def test_fragmented_recorder_and_failed_begin_cleanup():
    executable = Path(__file__).resolve().parents[1] / "build/record-memory-tests/zephyr/zephyr.exe"
    result = subprocess.run([executable], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PROJECT EXECUTION SUCCESSFUL" in result.stdout
