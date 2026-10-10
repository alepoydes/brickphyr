# SPDX-License-Identifier: Apache-2.0
"""Check a linked Prime F4 image against its retained bootloader contract."""

import argparse
from pathlib import Path
from struct import unpack_from

from elftools.elf.elffile import ELFFile


def check_image(elf_path: Path, image: bytes) -> None:
    # Independent of the linker configuration: these bounds belong to the hub.
    origin, limit = 0x08010000, 0x08100000
    if not 0x284 <= len(image) <= limit - origin:
        raise ValueError("Image does not fit the Prime F4 main-firmware region")

    stack, entry = unpack_from("<II", image)
    if stack % 8 or not 0x20000000 < stack <= 0x20050000:
        raise ValueError("Initial stack is outside aligned Prime F4 SRAM")
    if not entry & 1 or not origin + 0x284 <= entry - 1 < origin + len(image):
        raise ValueError("Reset vector is not Thumb code inside the image")

    for offset in (0x1F0, 0x264):
        color, mcu, reserved, board = unpack_from("<B15sB15s", image, offset)
        if (
            color >= 0x0F
            or reserved != 0
            or mcu.rstrip(b"\0") != b"STM32F413"
            or board.rstrip(b"\0") != b"SPIKE Prime"
        ):
            raise ValueError(f"Invalid Prime F4 boot info at {offset:#x}")

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
    args = parser.parse_args()
    try:
        image = args.bin.read_bytes()
        check_image(args.elf, image)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Image check failed: {error}\n")
    print(f"Prime F4 image layout OK: {len(image)} bytes; hardware remains unverified")
