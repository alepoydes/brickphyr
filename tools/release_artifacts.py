# SPDX-License-Identifier: Apache-2.0
"""Bundle one already-built ARM image with its notices and provenance."""

import argparse
import hashlib
import io
import json
import re
import subprocess
import sys
import tarfile
from pathlib import Path
from tempfile import NamedTemporaryFile
from zipfile import BadZipFile, ZipFile

import yaml
from west.manifest import Manifest

from tools import workspace_status
from tools.build_record import verify
from tools.package_firmware import license_notice

ROOT = Path(__file__).resolve().parents[1]
ARM_REQUIREMENTS = {
    "CONFIG_CPU_CORTEX_M4=y", "CONFIG_FPU=y", "CONFIG_FP_HARDABI=y", "CONFIG_ARM_MPU=y",
}


def git(cwd: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(cwd), *args], text=True).strip()


def source_provenance() -> dict:
    manifest = Manifest.from_topdir()
    frozen = manifest.as_frozen_dict(active_only=True)
    pybricks = manifest.get_projects(["pybricks"])[0]
    nested = Path(pybricks.abspath) / "micropython"
    nested_url = git(Path(pybricks.abspath), "config", "--file", ".gitmodules",
                     "--get", "submodule.micropython.url")
    root_url = git(ROOT, "remote", "get-url", "origin")
    urls = [root_url, nested_url, *(p["url"] for p in frozen["manifest"]["projects"])]
    if any(not url.startswith(("https://", "git@")) for url in urls):
        raise ValueError("Release source URLs must use HTTPS or SSH")
    return {
        "root_head": git(ROOT, "rev-parse", "HEAD"), "root_url": root_url,
        "west_manifest": frozen,
        "micropython": {"path": f"{pybricks.path}/micropython", "url": nested_url,
                        "revision": git(nested, "rev-parse", "HEAD")},
    }


def bundle_files(build: Path, output: Path, name: str, version: str,
                 notice: str, provenance: dict) -> Path:
    """Validate real build artifacts and atomically bundle their bytes."""
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name):
        raise ValueError("NAME must be one safe archive directory name")
    build_record = verify(build, current=False, clean=True)
    if build_record["state"]["sources"]["brickphyr"]["revision"] != provenance["root_head"]:
        raise ValueError("Build source differs from requested provenance")
    if build_record["state"]["version"] != version:
        raise ValueError("Build version differs from requested release version")
    config_bytes = (build / "zephyr/.config").read_bytes()
    config = set(config_bytes.decode().splitlines())
    if not ARM_REQUIREMENTS <= config:
        raise ValueError("Release image needs Cortex-M4, MPU and hard-float configuration")
    f4 = "CONFIG_BOARD_SPIKE_PRIME_F4=y" in config
    qemu = "CONFIG_BOARD_MPS2_AN386=y" in config
    if f4 == qemu or (f4 and "CONFIG_SOC_SERIES_STM32F4X=y" not in config):
        raise ValueError("Only SPIKE F4 and mps2/an386 images are supported")

    files = {
        "zephyr.elf": (build / "zephyr/zephyr.elf").read_bytes(),
        "firmware.config": config_bytes,
        "zephyr.dts": (build / "zephyr/zephyr.dts").read_bytes(),
        "ReadMe_OSS.txt": notice.encode(),
    }
    if f4:
        firmware = build / "zephyr/firmware.zip"
        with ZipFile(firmware) as archive:
            metadata = json.loads(archive.read("firmware.metadata.json"))
            if metadata.get("firmware-version") != version:
                raise ValueError("Firmware ZIP version differs from requested release version")
            if archive.read("firmware-base.bin") != (build / "zephyr/zephyr.bin").read_bytes():
                raise ValueError("Firmware ZIP payload differs from zephyr.bin")
        files["firmware.zip"] = firmware.read_bytes()
    if "CONFIG_LLEXT_EDK=y" in config:
        sdk_archive = build / "zephyr/brickphyr-sdk.tar.xz"
        with tarfile.open(sdk_archive, "r:xz") as archive:
            member = archive.extractfile("brickphyr-sdk/build.json")
            if member is None:
                raise ValueError("SDK build.json is missing")
            sdk_info = json.load(member)
        if sdk_info != build_record:
            raise ValueError("SDK build metadata differs from the linked firmware")
        files["brickphyr-sdk.tar.xz"] = sdk_archive.read_bytes()

    record = {**provenance, "name": name, "release_version": version, "build": build_record,
              "sha256": {key: hashlib.sha256(value).hexdigest()
                         for key, value in sorted(files.items())}}
    files["provenance.json"] = (json.dumps(record, indent=2, sort_keys=True) + "\n").encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=output.parent, prefix=f".{output.name}.", delete=False) as temp:
        temporary = Path(temp.name)
    try:
        with tarfile.open(temporary, "w:xz") as archive:
            for filename, data in sorted(files.items()):
                member = tarfile.TarInfo(f"{name}/{filename}")
                member.size = len(data)
                member.uid = member.gid = member.mtime = 0
                member.uname = member.gname = ""
                member.mode = 0o755 if filename == "zephyr.elf" else 0o644
                archive.addfile(member, io.BytesIO(data))
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return output


def package(build: Path, output: Path, name: str, version: str) -> Path:
    if workspace_status.main(["--check"]):
        raise ValueError("West workspace differs from clean manifest pins")
    verify(build, clean=True)
    info = yaml.safe_load((build / "build_info.yml").read_text())
    sdk = Path(info["cmake"]["toolchain"]["path"])
    provenance = source_provenance()
    sdk_version = (sdk / "sdk_version").read_text().strip()
    zephyr = Path(info["cmake"]["zephyr"]["zephyr-base"])
    if sdk_version != (zephyr / "SDK_VERSION").read_text().strip():
        raise ValueError("Build toolchain differs from Zephyr's required SDK version")
    provenance["zephyr_sdk_version"] = sdk_version
    provenance["elf_sections"] = subprocess.check_output(
        [str(sdk / "gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-size"), "-A", "zephyr.elf"],
        cwd=build / "zephyr", text=True,
    )
    config = set((build / "zephyr/.config").read_text().splitlines())
    notice = "Source-component and runtime license texts.\n" + license_notice(config, sdk)
    return bundle_files(build, output, name, version, notice, provenance)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args(argv)
    try:
        result = package(args.build, args.output, args.name, args.version)
    except (OSError, ValueError, RuntimeError, KeyError, BadZipFile, subprocess.CalledProcessError,
            tarfile.TarError) as error:
        parser.exit(1, f"Release packaging failed: {error}\n")
    print(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
