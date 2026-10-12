# SPDX-License-Identifier: Apache-2.0
"""Persist user data across fresh firmware processes on actual LittleFS."""

import subprocess
from pathlib import Path


def test_user_data_persistence_and_failures(tmp_path):
    executable = Path(__file__).resolve().parents[1] / "build/app-data-tests/zephyr/zephyr.exe"
    flash = tmp_path / "flash.bin"
    flash.write_bytes(b"\xff" * (2 * 1024 * 1024))
    for marker in (
        "USER DATA SAVED",
        "SAVED DATA RELOADED; CORRUPTION PREPARED",
        "CORRUPT DATA REJECTED",
    ):
        before = flash.read_bytes()
        result = subprocess.run(
            [executable, f"--flash={flash}"], capture_output=True, text=True, timeout=5
        )
        assert result.returncode == 0, result.stdout + result.stderr
        assert marker in result.stdout, result.stdout
        if marker == "CORRUPT DATA REJECTED":
            assert flash.read_bytes() == before
    foreign = b"\xa5" * (2 * 1024 * 1024)
    flash.write_bytes(foreign)
    result = subprocess.run(
        [executable, f"--flash={flash}"], capture_output=True, text=True, timeout=5
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "FOREIGN DATA STORE REJECTED" in result.stdout
    assert flash.read_bytes() == foreign
