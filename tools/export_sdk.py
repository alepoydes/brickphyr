# SPDX-License-Identifier: Apache-2.0
"""Extend Zephyr's EDK with the matching Python compiler and native module tools."""

import argparse
import shutil
import tarfile
from pathlib import Path
from tempfile import TemporaryDirectory

import yaml


def export(build: Path, micropython: Path | None):
    info = yaml.safe_load((build / "build_info.yml").read_text())
    edk = Path(info["cmake"]["llext-edk"]["file"])
    output = build / "zephyr/brickphyr-sdk.tar.xz"
    with TemporaryDirectory(dir=build) as temporary:
        stage = Path(temporary)
        with tarfile.open(edk) as archive:
            archive.extractall(stage, filter="data")
        roots = list(stage.iterdir())
        if len(roots) != 1 or not roots[0].is_dir():
            raise ValueError("Expected one directory in the Zephyr EDK")
        sdk = roots[0]
        if micropython:
            python = sdk / "python"
            python.mkdir()
            for name in ("mpy-cross", "compiler.json"):
                shutil.copy2(build / "mpy-cross" / name, python / name)
            (python / "py").mkdir()
            for header in (micropython / "py").glob("*.h"):
                shutil.copy2(header, python / "py" / header.name)
            for name in ("dynruntime.mk", "makeqstrdata.py"):
                shutil.copy2(micropython / "py" / name, python / "py" / name)
            (python / "tools").mkdir()
            for name in ("mpy-tool.py", "mpy_ld.py", "ar_util.py"):
                shutil.copy2(micropython / "tools" / name, python / "tools" / name)
            shutil.copy2(micropython / "LICENSE", python / "LICENSE")

        def normalized(member):
            member.uid = member.gid = member.mtime = 0
            member.uname = member.gname = ""
            return member

        staged_output = stage / output.name
        with tarfile.open(staged_output, "w:xz") as archive:
            archive.add(sdk, arcname="brickphyr-sdk", filter=normalized)
        staged_output.replace(output)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--micropython", type=Path)
    args = parser.parse_args()
    print(export(args.build, args.micropython))
