# SPDX-License-Identifier: Apache-2.0
"""Verify an actual F4 package through Pybricks, without accessing USB."""

import shutil
from pathlib import Path
from struct import pack_into

import pytest
from west.manifest import Manifest

from tools.package_firmware import package


def test_package_roundtrip_and_rejection(tmp_path, monkeypatch):
    build = Path(__file__).resolve().parents[1] / "build/prime"
    for name in ("zephyr.elf", "zephyr.bin", "zephyr.map"):
        shutil.copyfile(build / "zephyr" / name, tmp_path / name)
    # Use the SDK selected for the compiled artifact, not a workstation default.
    sdk = next(
        line.split("=", 1)[1]
        for line in (build / "CMakeCache.txt").read_text().splitlines()
        if line.startswith("ZEPHYR_SDK_INSTALL_DIR:")
    )
    pybricks = Manifest.from_topdir().get_projects(["pybricks"])[0]
    monkeypatch.syspath_prepend(str(Path(pybricks.abspath) / "tools/flash"))
    from firmware import create_firmware_blob

    output = package(tmp_path, "test", Path(sdk))
    _, variants = create_firmware_blob(output)
    image = (tmp_path / "zephyr.bin").read_bytes()
    assert set(variants) == {"prime_hub_f4"}
    assert bytes(variants["prime_hub_f4"]) == image

    previous_package = output.read_bytes()
    corrupt = bytearray(image)
    pack_into("<I", corrupt, 4, 0x08008001)
    (tmp_path / "zephyr.bin").write_bytes(corrupt)
    with pytest.raises(ValueError, match="Reset vector"):
        package(tmp_path, "test", Path(sdk))
    assert output.read_bytes() == previous_package
