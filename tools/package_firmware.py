# SPDX-License-Identifier: Apache-2.0
"""Package a checked F4 image using Pybricks' metadata generator."""

import argparse
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from west.manifest import Manifest

from tools.check_image import check_image


def package(build: Path, version: str, sdk: Path) -> Path:
    image = (build / "zephyr.bin").read_bytes()
    check_image(build / "zephyr.elf", image)
    manifest = Manifest.from_topdir()
    pybricks = Path(manifest.get_projects(["pybricks"])[0].abspath)
    output = build / "firmware.zip"
    root = Path(__file__).resolve().parents[1]
    notices = [
        ("Brickphyr", root / "LICENSE"),
        ("Pybricks MIT helpers", root / "LICENSES/Pybricks-MIT.txt"),
    ]
    for project_name, license_path in (
        ("zephyr", "LICENSE"),
        ("cmsis_6", "LICENSE"),
        ("hal_stm32", "stm32cube/stm32f4xx/drivers/LICENSE.md"),
        ("hal_stm32", "stm32cube/stm32f4xx/soc/LICENSE.md"),
        ("littlefs", "LICENSE.md"),
        ("zcbor", "LICENSE"),
    ):
        project = manifest.get_projects([project_name])[0]
        notices.append((f"{project_name}: {license_path}", Path(project.abspath) / license_path))
    for license_path in (
        "picolibc/COPYING.picolibc",
        "picolibc/COPYING.NEWLIB",
        "gcc/COPYING3",
        "gcc/COPYING.RUNTIME",
    ):
        notices.append((license_path, sdk / "gnu/arm-zephyr-eabi/share/licenses" / license_path))
    notice = "Experimental Brickphyr F4 firmware. Source-component and runtime license texts.\n"
    notice += "\n\n".join(f"{name}\n\n{path.read_text()}" for name, path in notices)

    with TemporaryDirectory(dir=build) as directory:
        temporary = Path(directory)
        metadata = temporary / "firmware.metadata.json"
        subprocess.run(
            [
                sys.executable,
                str(pybricks / "tools/metadata.py"),
                version,
                "prime_hub_f4",
                str(build / "zephyr.map"),
                str(metadata),
                "--no-custom-name",
            ],
            check=True,
        )
        archive_path = temporary / "firmware.zip"
        with ZipFile(archive_path, "w") as archive:
            for name, data in (
                ("firmware-base.bin", image),
                (metadata.name, metadata.read_bytes()),
                ("ReadMe_OSS.txt", notice.encode()),
            ):
                archive.writestr(ZipInfo(name), data, compress_type=ZIP_DEFLATED)
        archive_path.replace(output)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "build", type=Path, help="Zephyr output directory containing ELF, BIN and MAP"
    )
    parser.add_argument("--version", required=True, help="version of the built firmware")
    parser.add_argument("--sdk", required=True, type=Path, help="Zephyr SDK used for the build")
    args = parser.parse_args()
    try:
        output = package(args.build.resolve(), args.version, args.sdk.resolve())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Packaging failed: {error}\n")
    print(f"Created {output}")
