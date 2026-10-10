# SPDX-License-Identifier: Apache-2.0
"""Exercise the actual filesystem/settings stack and refusal to erase foreign data."""

import subprocess
from pathlib import Path


def test_settings_storage(tmp_path):
    executable = Path(__file__).resolve().parents[1] / "build/storage-tests/zephyr/zephyr.exe"
    for fill, marker in ((255, "SETTINGS SAVE AND RELOAD VERIFIED"), (165, "FOREIGN STORAGE REJECTED")):
        flash = tmp_path / f"flash-{fill}.bin"
        initial = bytes((fill,)) * (2 * 1024 * 1024)
        flash.write_bytes(initial)
        result = subprocess.run(
            [executable, f"--flash={flash}"], capture_output=True, text=True, timeout=5
        )
        assert result.returncode == 0, result.stdout + result.stderr
        assert marker in result.stdout, result.stdout
        if fill != 255:
            assert flash.read_bytes() == initial, "Unrecognized storage was modified"
