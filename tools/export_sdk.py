# SPDX-License-Identifier: Apache-2.0
"""Export a portable native SDK from Zephyr's EDK and optional Python tools."""

import argparse
import json
import re
import shutil
import tarfile
from pathlib import Path
from tempfile import TemporaryDirectory

import yaml

from tools.build_record import verify


def portable_cflags(edk: Path, configured: list[str], dsp: bool) -> str:
    source = (edk / "cmake.cflags").read_text()
    match = re.search(r'^set\(LLEXT_CFLAGS "([^"\n]*)"\)$', source, re.M)
    if not match:
        raise ValueError("Missing LLEXT_CFLAGS in Zephyr EDK")
    flags = match.group(1).split(";")
    prefixes = ("-mcpu=", "-march=", "-mthumb", "-mabi=", "-mfpu=",
                "-mfloat-abi=", "-mfp16-format=", "-mtp=", "-mlong-calls",
                "-fno-asynchronous-unwind-tables", "-specs=picolibc.specs")
    selected = list(dict.fromkeys(flag for flag in flags if flag.startswith(prefixes)))
    for prefix in ("-mcpu=", "-march=", "-mabi=", "-mfpu=", "-mfloat-abi="):
        actual = [flag for flag in selected if flag.startswith(prefix)]
        expected = [flag for flag in configured if flag.startswith(prefix)]
        if actual != expected:
            raise ValueError(f"Stale Zephyr EDK architecture flags: {prefix}")
    if not any(flag.startswith(("-mcpu=", "-march=")) for flag in selected) or "-mthumb" not in selected:
        raise ValueError("Missing ARM CPU or Thumb flag in Zephyr EDK")
    selected.append("-I${CMAKE_CURRENT_LIST_DIR}/include")
    if dsp:
        selected.extend(("-I${CMAKE_CURRENT_LIST_DIR}/include/cmsis-dsp",
                         "-I${CMAKE_CURRENT_LIST_DIR}/include/cmsis-core"))
    return "\n".join(("# Portable flags selected from Zephyr's generated LLEXT_CFLAGS.",
                      "set(LLEXT_CFLAGS", *(f'  "{flag}"' for flag in selected), ")", ""))


def export(build: Path, micropython: Path | None, dsp: list[Path] | None):
    provenance = verify(build)
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
        edk_root = roots[0]
        sdk = stage / "brickphyr-sdk"
        sdk.mkdir()
        root = Path(__file__).resolve().parents[1]
        shutil.copy2(root / "LICENSE", sdk / "LICENSE")
        (sdk / "build.json").write_text(json.dumps(provenance, indent=2) + "\n")
        (sdk / "tools").mkdir()
        shutil.copy2(root / "brickphyrdev/package.py", sdk / "tools/package.py")
        shutil.copy2(root / "sdk/package.cmake", sdk / "package.cmake")
        headers = sdk / "include"
        headers.mkdir()
        public = Path(__file__).resolve().parents[1] / "sdk/include"
        for header in public.glob("*.h"):
            shutil.copy2(header, headers / header.name)
        shutil.copy2(build / "zephyr/include/generated/brickphyr_api.h", headers / "brickphyr_api.h")
        (sdk / "cmake.cflags").write_text(portable_cflags(
            edk_root, info["cmake"]["llext-edk"]["cflags"], bool(dsp)))
        if dsp:
            shutil.copytree(edk_root / "include/modules/lib/cmsis-dsp/Include",
                            headers / "cmsis-dsp")
            shutil.copytree(edk_root / "include/modules/hal/cmsis_6/CMSIS/Core/Include",
                            headers / "cmsis-core")
            (sdk / "lib").mkdir(exist_ok=True)
            shutil.copy2(dsp[0], sdk / "lib/libcmsis-dsp.a")
            (sdk / "LICENSES").mkdir(exist_ok=True)
            shutil.copy2(dsp[1], sdk / "LICENSES/CMSIS-DSP.txt")
            core_license = Path(info["west"]["topdir"]) / "modules/hal/cmsis_6/LICENSE"
            shutil.copy2(core_license, sdk / "LICENSES/CMSIS-Core.txt")
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
    parser.add_argument("--dsp", type=Path, nargs=2, metavar=("ARCHIVE", "LICENSE"))
    args = parser.parse_args()
    print(export(args.build, args.micropython, args.dsp))
