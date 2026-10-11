/* SPDX-License-Identifier: Apache-2.0 */
/* ARMv7-M leaves three application MPU regions after kernel/stack protection.
 * Constants share RX memory with code; data and zero-filled BSS remain RW/NX.
 * The ABI identity is file metadata and consumes no application MPU region. */
SECTIONS
{
	.text 0 : { *(.text .text.*) *(.rodata .rodata.*) }
	.data 0 : { *(.data .data.*) }
	.bss 0 : { *(.bss .bss.*) *(COMMON) }
	.preinit_array 0 : { *(.preinit_array .preinit_array.*) }
	.init_array 0 : { *(.init_array .init_array.*) }
	.fini_array 0 : { *(.fini_array .fini_array.*) }
	.brickphyr.abi 0 (INFO) : { KEEP(*(.brickphyr.abi)) }
	.ARM.attributes 0 : { *(.ARM.attributes) }
}
