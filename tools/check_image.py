# SPDX-License-Identifier: Apache-2.0
"""Check a linked hub image against its retained bootloader contract."""

import argparse
from pathlib import Path

from elftools.elf.elffile import ELFFile

from brickphyrdev.firmware import check_binary


def check_image(elf_path: Path, image: bytes, platform: str = "prime_hub_f4") -> None:
    origin, limit, sram_start, sram_end, entry = check_binary(image, platform)

    with elf_path.open("rb") as stream:
        elf = ELFFile(stream)
        if (
            elf.elfclass != 32
            or not elf.little_endian
            or elf["e_machine"] != "EM_ARM"
            or elf["e_type"] != "ET_EXEC"
        ):
            raise ValueError("Expected a linked little-endian ARM ELF32 image")
        if elf["e_entry"] != entry:
            raise ValueError("Reset vector disagrees with ELF entry point")

        start, end = limit, origin
        for segment in elf.iter_segments():
            if platform == "technic_hub" and segment["p_type"] == "PT_LOAD" and segment["p_memsz"]:
                address = segment["p_vaddr"]
                stop = address + segment["p_memsz"]
                if not (
                    origin <= address < stop <= limit or sram_start <= address < stop <= sram_end
                ):
                    raise ValueError("ELF runtime memory is outside Technic firmware/SRAM")
            if segment["p_type"] != "PT_LOAD" or not segment["p_filesz"]:
                continue
            address = segment["p_paddr"]
            stop = address + segment["p_filesz"]
            if not origin <= address < stop <= limit:
                raise ValueError("ELF load segment would overwrite a reserved region")
            # objcopy fills gaps between sections with 0xff; ELF segment padding
            # need not have that value. Compare the actual loadable sections.
            for section in elf.iter_sections():
                if (
                    not section["sh_flags"] & 2
                    or section["sh_type"] == "SHT_NOBITS"
                    or not segment.section_in_segment(section)
                ):
                    continue
                offset = address - origin + section["sh_addr"] - segment["p_vaddr"]
                if image[offset : offset + section["sh_size"]] != section.data():
                    raise ValueError(f"Binary differs from ELF section {section.name}")
            start, end = min(start, address), max(end, stop)

        if start != origin or end != origin + len(image):
            raise ValueError("Binary extent disagrees with ELF load segments")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("bin", type=Path)
    parser.add_argument(
        "--platform", choices=("prime_hub_f4", "technic_hub"), default="prime_hub_f4"
    )
    args = parser.parse_args()
    try:
        image = args.bin.read_bytes()
        check_image(args.elf, image, args.platform)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Image check failed: {error}\n")
    print(f"{args.platform} image layout OK: {len(image)} bytes; hardware remains unverified")
