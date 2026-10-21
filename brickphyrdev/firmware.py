# SPDX-License-Identifier: Apache-2.0
"""Shared binary checks for building and installing Brickphyr firmware."""

from struct import unpack_from


def check_binary(image: bytes, platform: str = "prime_hub_f4") -> tuple:
    # Independent of the linker configuration: these bounds belong to the hub.
    if platform == "prime_hub_f4":
        origin, limit, minimum = 0x08010000, 0x08100000, 0x284
        sram_start, sram_end = 0x20000000, 0x20050000
    elif platform == "technic_hub":
        origin, limit, minimum = 0x08008000, 0x0803C000, 8
        # The factory loader's selector at 0x20000200 must not be C runtime RAM.
        sram_start, sram_end = 0x20000400, 0x20010000
        if len(image) % 4:
            raise ValueError("Technic base image must be word aligned for its checksum")
        # The installer appends one checksum word; keep it out of user storage.
        limit -= 4
    else:
        raise ValueError(f"Unsupported platform: {platform}")
    if not minimum <= len(image) <= limit - origin:
        raise ValueError(f"Image does not fit the {platform} main-firmware region")

    stack, entry = unpack_from("<II", image)
    if stack % 8 or not sram_start < stack <= sram_end:
        raise ValueError(f"Initial stack is outside aligned {platform} runtime SRAM")
    if not entry & 1 or not origin + minimum <= entry - 1 < origin + len(image):
        raise ValueError("Reset vector is not Thumb code inside the image")

    for offset in (0x1F0, 0x264) if platform == "prime_hub_f4" else ():
        color, mcu, reserved, board = unpack_from("<B15sB15s", image, offset)
        if (
            color >= 0x0F
            or reserved != 0
            or mcu.rstrip(b"\0") != b"STM32F413"
            or board.rstrip(b"\0") != b"SPIKE Prime"
        ):
            raise ValueError(f"Invalid Prime F4 boot info at {offset:#x}")

    return origin, limit, sram_start, sram_end, entry
