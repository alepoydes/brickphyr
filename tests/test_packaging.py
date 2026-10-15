# SPDX-License-Identifier: Apache-2.0
"""Verify built hub packages through Pybricks, without accessing hardware."""

import shutil
from pathlib import Path
from struct import pack_into
from zipfile import ZipFile

import pytest
from west.manifest import Manifest

from tools.package_firmware import license_notice, package


@pytest.mark.parametrize(
    "directory,platform", [("prime", "prime_hub_f4"), ("technic-base", "technic_hub")]
)
def test_package_roundtrip_and_rejection(tmp_path, monkeypatch, directory, platform):
    build = Path(__file__).resolve().parents[1] / "build" / directory
    for name in ("zephyr.elf", "zephyr.bin", "zephyr.map", ".config"):
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
    with ZipFile(output) as archive:
        notice = archive.read("ReadMe_OSS.txt").decode()
    stm32 = "stm32f4xx" if platform == "prime_hub_f4" else "stm32l4xx"
    assert f"Experimental Brickphyr {platform} firmware." in notice
    assert "Pybricks MIT helpers" in notice
    assert f"hal_stm32: stm32cube/{stm32}/drivers/LICENSE.md" in notice
    assert f"hal_stm32: stm32cube/{stm32}/soc/LICENSE.md" in notice
    assert "picolibc/COPYING.picolibc" in notice
    if platform == "prime_hub_f4":
        qemu_notice = license_notice({"CONFIG_BRICKPHYR_BLE_PEER=y"}, Path(sdk))
        assert "zephyr: LICENSE" in qemu_notice
        assert "cmsis_6: LICENSE" in qemu_notice
        assert "hal_stm32:" not in qemu_notice
        assert "SDL Xbox report codec" in qemu_notice
        assert "Bluepad32 Xbox rumble report fields" in qemu_notice
        assert "80d6314e46f871719fbb4b721a2cc02227c4f369" in qemu_notice
        assert "Copyright 2019 Ricardo Quesada (Apache-2.0)" in qemu_notice

    _, variants = create_firmware_blob(output)
    image = (tmp_path / "zephyr.bin").read_bytes()
    assert set(variants) == {platform}
    blob = bytes(variants[platform])
    assert blob[: len(image)] == image
    if platform == "technic_hub":
        assert len(blob) == len(image) + 4
        # Factory-loader invariant across the programmed image and erased tail.
        checked = blob + b"\xff" * (0x37000 - len(blob))
        assert (
            sum(int.from_bytes(checked[i : i + 4], "little") for i in range(0, len(checked), 4))
            & 0xFFFFFFFF
            == 0
        )
    else:
        assert blob == image

    previous_package = output.read_bytes()
    corrupt = bytearray(image)
    pack_into("<I", corrupt, 4, 0x08000001)
    (tmp_path / "zephyr.bin").write_bytes(corrupt)
    with pytest.raises(ValueError, match="Reset vector"):
        package(tmp_path, "test", Path(sdk))
    assert output.read_bytes() == previous_package
