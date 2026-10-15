# SPDX-License-Identifier: Apache-2.0
"""Package a checked hub image using Pybricks' metadata generator."""

import argparse
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from west.manifest import Manifest

from tools.check_image import check_image


def license_notice(config: set[str], sdk: Path) -> str:
    manifest = Manifest.from_topdir()
    root = Path(__file__).resolve().parents[1]
    notices = [
        ("Brickphyr", root / "LICENSE"),
        ("Pybricks MIT helpers", root / "LICENSES/Pybricks-MIT.txt"),
    ]
    if "CONFIG_BRICKPHYR_IMU=y" in config:
        notices.append(("ST/Pybricks IMU register helpers", root / "LICENSES/ST-LSM6DS3TR-C.txt"))
    if config & {"CONFIG_BRICKPHYR_IMU=y", "CONFIG_BRICKPHYR_PORT_UART=y"}:
        notices.append(("Pybricks continuation macros", root / "LICENSES/Pybricks-BSD.txt"))
    if config & {"CONFIG_BRICKPHYR_MOTOR_CONTROL=y", "CONFIG_BRICKPHYR_PYBRICKS=y"}:
        notices.append(("LEGO PBIO computation", root / "LICENSES/LEGO-BSD-3-Clause.txt"))
    if "CONFIG_BRICKPHYR_BLE_PEER=y" in config:
        notices.append(("SDL Xbox report codec", root / "LICENSES/SDL-Zlib.txt"))
        notices.append((
            "Bluepad32 Xbox rumble report fields; Copyright 2019 "
            "Ricardo Quesada (Apache-2.0). Source: "
            "https://github.com/ricardoquesada/bluepad32/blob/"
            "80d6314e46f871719fbb4b721a2cc02227c4f369/"
            "src/components/bluepad32/parser/uni_hid_parser_xboxone.c",
            root / "LICENSE",
        ))
    if "CONFIG_BRICKPHYR_PYTHON=y" in config:
        notices.append(("MicroPython core", root / "LICENSES/MicroPython-MIT.txt"))
    if "CONFIG_BRICKPHYR_CC2640_CONTROL=y" in config:
        notices.append(("TI CC2640 protocol definitions", root / "LICENSES/TI-BSD-3-Clause.txt"))
    dependencies = [
        ("zephyr", "LICENSE"),
        ("cmsis_6", "LICENSE"),
    ]
    stm32 = None
    if "CONFIG_SOC_SERIES_STM32F4X=y" in config:
        stm32 = "stm32f4xx"
    elif "CONFIG_SOC_SERIES_STM32L4X=y" in config:
        stm32 = "stm32l4xx"
    if stm32:
        dependencies.extend((
            ("hal_stm32", f"stm32cube/{stm32}/drivers/LICENSE.md"),
            ("hal_stm32", f"stm32cube/{stm32}/soc/LICENSE.md"),
        ))
    for symbol, project_name, license_path in (
        ("FILE_SYSTEM_LITTLEFS", "littlefs", "LICENSE.md"),
        ("ZCBOR", "zcbor", "LICENSE"),
        ("MBEDTLS", "mbedtls", "LICENSE"),
        ("PSA_CRYPTO", "tf-psa-crypto", "LICENSE"),
        ("BT_HCI_SETUP", "ti-bt-service-packs", "LICENSE"),
        ("CMSIS_DSP", "cmsis-dsp", "LICENSE"),
    ):
        if f"CONFIG_{symbol}=y" in config:
            dependencies.append((project_name, license_path))
    for project_name, license_path in dependencies:
        project = manifest.get_projects([project_name])[0]
        notices.append((f"{project_name}: {license_path}", Path(project.abspath) / license_path))
    for license_path in (
        "picolibc/COPYING.picolibc",
        "picolibc/COPYING.NEWLIB",
        "gcc/COPYING3",
        "gcc/COPYING.RUNTIME",
    ):
        notices.append((license_path, sdk / "gnu/arm-zephyr-eabi/share/licenses" / license_path))
    return "\n\n".join(f"{name}\n\n{path.read_text()}" for name, path in notices)


def package(build: Path, version: str, sdk: Path) -> Path:
    config = set((build / ".config").read_text().splitlines())
    if "CONFIG_BOARD_SPIKE_PRIME_F4=y" in config:
        platform = "prime_hub_f4"
        metadata_options = []
    elif "CONFIG_BOARD_TECHNIC_HUB=y" in config:
        platform = "technic_hub"
        # The factory loader checks firmware plus the first 12 KiB of user
        # storage, assuming the unwritten tail was erased by its installer.
        metadata_options = ["--checksum-size", "0x37000"]
    else:
        raise ValueError("No supported hub board in the build configuration")
    image = (build / "zephyr.bin").read_bytes()
    check_image(build / "zephyr.elf", image, platform)
    manifest = Manifest.from_topdir()
    pybricks = Path(manifest.get_projects(["pybricks"])[0].abspath)
    output = build / "firmware.zip"
    notice = (
        f"Experimental Brickphyr {platform} firmware. Source-component and runtime license texts.\n"
        + license_notice(config, sdk)
    )

    with TemporaryDirectory(dir=build) as directory:
        temporary = Path(directory)
        metadata = temporary / "firmware.metadata.json"
        subprocess.run(
            [
                sys.executable,
                str(pybricks / "tools/metadata.py"),
                version,
                platform,
                str(build / "zephyr.map"),
                str(metadata),
                "--no-custom-name",
                *metadata_options,
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
