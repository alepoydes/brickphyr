/* SPDX-License-Identifier: Apache-2.0 */

ASSERT(_vector_end - _vector_start <= 0x1f0, "Vector table overlaps Prime F4 boot info");
. = 0x1f0;
KEEP(*(.boot_info1))
. = 0x264;
KEEP(*(.boot_info2))
