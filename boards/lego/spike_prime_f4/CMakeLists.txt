# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources(board.c)
zephyr_linker_sources(ROM_START SORT_KEY 0x2boot_info boot_info.ld)
