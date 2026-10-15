# SPDX-License-Identifier: Apache-2.0
"""Check release archives against real firmware ZIP and SDK TAR members."""

import hashlib
import io
import json
import subprocess
import tarfile
from copy import deepcopy
from zipfile import ZipFile

import pytest

from tools.build_record import source_state
from tools.release_artifacts import bundle_files


def test_dirty_source_fingerprint(tmp_path):
    def git(*args):
        subprocess.run(["git", "-C", str(tmp_path), *args], check=True, capture_output=True)

    header = tmp_path / "api.h"
    header.write_text("original")
    git("init")
    git("add", "api.h")
    git("-c", "user.name=Test", "-c", "user.email=test@localhost",
        "-c", "commit.gpgsign=false", "commit", "-m", "fixture")
    clean = source_state(tmp_path)
    header.write_text("first edit")
    first = source_state(tmp_path)
    header.write_text("second edit")
    second = source_state(tmp_path)
    extra = tmp_path / "new header.h"
    extra.write_text("new API")
    third = source_state(tmp_path)
    extra.write_text("changed API")
    fourth = source_state(tmp_path)
    assert not clean["dirty"] and all(s["dirty"] for s in (first, second, third, fourth))
    assert len({s["changes_sha256"] for s in (first, second, third, fourth)}) == 4
    header.write_text("original")
    extra.unlink()
    assert source_state(tmp_path) == clean


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
    elf = b"\x7fELF synthetic"
    binary = b"firmware bytes"
    dts = b"/dts-v1/;\n"
    (zephyr / "zephyr.elf").write_bytes(elf)
    (zephyr / "zephyr.bin").write_bytes(binary)
    (zephyr / "zephyr.dts").write_bytes(dts)
    provenance = {"root_head": "a" * 40, "projects": [], "zephyr_sdk_version": "1.0.1"}

    def digest(data):
        return hashlib.sha256(data).hexdigest()

    build_record = {
        "state": {
            "version": version,
            "sources": {"brickphyr": {"revision": provenance["root_head"], "dirty": False}},
            "config_sha256": digest(config),
            "dts_sha256": digest(dts),
        },
        "artifacts": {"zephyr.elf": digest(elf), "zephyr.bin": digest(binary)},
    }
    (zephyr / "brickphyr-build.json").write_text(json.dumps(build_record))

    def write_firmware(payload=b"firmware bytes", firmware_version=version):
        with ZipFile(zephyr / "firmware.zip", "w") as archive:
            archive.writestr("firmware-base.bin", payload)
            archive.writestr("firmware.metadata.json",
                             json.dumps({"firmware-version": firmware_version}))

    def write_sdk(record=build_record):
        metadata = json.dumps(record).encode()
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
        assert {f"brickphyr/{name}" for name in (
            "zephyr.elf", "firmware.config", "zephyr.dts", "ReadMe_OSS.txt",
            "firmware.zip", "brickphyr-sdk.tar.xz", "provenance.json",
        )} <= names
        assert all((member.mtime, member.uid, member.gid) == (0, 0, 0)
                   for member in members)
        assert archive.extractfile("brickphyr/zephyr.elf").read() == b"\x7fELF synthetic"
        release_record = json.load(archive.extractfile("brickphyr/provenance.json"))
        assert release_record["release_version"] == version
        assert release_record["sha256"]["firmware.config"] == digest(config)
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
    dirty_sdk = deepcopy(build_record)
    dirty_sdk["state"]["sources"]["brickphyr"]["dirty"] = True
    write_sdk(dirty_sdk)
    with pytest.raises(ValueError, match="SDK build metadata differs"):
        bundle_files(build, output, "brickphyr", version, "notices\n", provenance)
    assert output.read_bytes() == previous

    old_config_sdk = deepcopy(build_record)
    old_config_sdk["state"]["config_sha256"] = digest(b"old configuration")
    write_sdk(old_config_sdk)
    with pytest.raises(ValueError, match="SDK build metadata differs"):
        bundle_files(build, output, "brickphyr", version, "notices\n", provenance)
    assert output.read_bytes() == previous

    # Re-exporting an SDK with matching metadata cannot bless an old ELF.
    (zephyr / "zephyr.elf").write_bytes(b"\x7fELF stale")
    write_sdk(build_record)
    with pytest.raises(ValueError, match="Build artifacts differ"):
        bundle_files(build, output, "brickphyr", version, "notices\n", provenance)
    assert output.read_bytes() == previous

    (zephyr / "zephyr.elf").write_bytes(elf)
    changed_provenance = {**provenance, "root_head": "b" * 40}
    with pytest.raises(ValueError, match="Build source differs"):
        bundle_files(build, output, "brickphyr", version, "notices\n", changed_provenance)
    assert output.read_bytes() == previous

    dirty_build = deepcopy(build_record)
    dirty_build["state"]["sources"]["brickphyr"]["dirty"] = True
    (zephyr / "brickphyr-build.json").write_text(json.dumps(dirty_build))
    write_sdk(dirty_build)
    with pytest.raises(ValueError, match="Build source is dirty"):
        bundle_files(build, output, "brickphyr", version, "notices\n", provenance)
    assert output.read_bytes() == previous

    wrong_version = deepcopy(build_record)
    wrong_version["state"]["version"] = "0.2.0-alpha.2"
    (zephyr / "brickphyr-build.json").write_text(json.dumps(wrong_version))
    write_sdk(wrong_version)
    with pytest.raises(ValueError, match="Build version differs"):
        bundle_files(build, output, "brickphyr", version, "notices\n", provenance)
    assert output.read_bytes() == previous
