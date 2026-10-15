# SPDX-License-Identifier: Apache-2.0
"""Check release archives against real firmware ZIP and SDK TAR members."""

import hashlib
import io
import json
import tarfile
from zipfile import ZipFile

import pytest

from tools.release_artifacts import bundle_files


def test_release_bundle_roundtrip_and_rejection(tmp_path):
    build = tmp_path / "build"
    zephyr = build / "zephyr"
    zephyr.mkdir(parents=True)
    version = "0.2.0-alpha.1"
    config = (
        "CONFIG_BOARD_SPIKE_PRIME_F4=y\n"
        "CONFIG_SOC_SERIES_STM32F4X=y\n"
        "CONFIG_CPU_CORTEX_M4=y\n"
        "CONFIG_ARM_MPU=y\nCONFIG_FPU=y\nCONFIG_FP_HARDABI=y\n"
        "CONFIG_LLEXT_EDK=y\n"
    ).encode()
    (zephyr / ".config").write_bytes(config)
    (zephyr / "zephyr.elf").write_bytes(b"\x7fELF synthetic")
    (zephyr / "zephyr.bin").write_bytes(b"firmware bytes")
    (zephyr / "zephyr.dts").write_text("/dts-v1/;\n")
    provenance = {"root_head": "a" * 40, "projects": [], "zephyr_sdk_version": "1.0.1"}

    def write_firmware(payload=b"firmware bytes", firmware_version=version):
        with ZipFile(zephyr / "firmware.zip", "w") as archive:
            archive.writestr("firmware-base.bin", payload)
            archive.writestr("firmware.metadata.json",
                             json.dumps({"firmware-version": firmware_version}))

    def write_sdk(dirty=False, sdk_config=config):
        metadata = json.dumps({
            "source": provenance["root_head"], "dirty": dirty,
            "config_sha256": hashlib.sha256(sdk_config).hexdigest(),
        }).encode()
        with tarfile.open(zephyr / "brickphyr-sdk.tar.xz", "w:xz") as archive:
            member = tarfile.TarInfo("brickphyr-sdk/build.json")
            member.size = len(metadata)
            archive.addfile(member, io.BytesIO(metadata))

    write_firmware()
    write_sdk()
    output = tmp_path / "release.tar.xz"
    bundle_files(build, output, "brickphyr", version, "notices\n", provenance)
    with tarfile.open(output, "r:xz") as archive:
        members = archive.getmembers()
        names = {member.name for member in members}
        assert names == {f"brickphyr/{name}" for name in (
            "zephyr.elf", "firmware.config", "zephyr.dts", "ReadMe_OSS.txt",
            "firmware.zip", "brickphyr-sdk.tar.xz", "provenance.json",
        )}
        assert all((member.mtime, member.uid, member.gid) == (0, 0, 0)
                   for member in members)
        assert archive.extractfile("brickphyr/zephyr.elf").read() == b"\x7fELF synthetic"
        record = json.load(archive.extractfile("brickphyr/provenance.json"))
        assert record["release_version"] == version
        assert record["sha256"]["firmware.config"] == hashlib.sha256(config).hexdigest()
    previous = output.read_bytes()

    write_firmware(payload=b"wrong payload")
    with pytest.raises(ValueError, match="payload differs"):
        bundle_files(build, output, "brickphyr", version, "notices\n", provenance)
    assert output.read_bytes() == previous

    write_firmware(firmware_version="wrong version")
    with pytest.raises(ValueError, match="version differs"):
        bundle_files(build, output, "brickphyr", version, "notices\n", provenance)
    assert output.read_bytes() == previous

    write_firmware()
    write_sdk(dirty=True)
    with pytest.raises(ValueError, match="SDK build metadata differs"):
        bundle_files(build, output, "brickphyr", version, "notices\n", provenance)
    assert output.read_bytes() == previous

    write_sdk(sdk_config=b"old configuration")
    with pytest.raises(ValueError, match="SDK build metadata differs"):
        bundle_files(build, output, "brickphyr", version, "notices\n", provenance)
    assert output.read_bytes() == previous
