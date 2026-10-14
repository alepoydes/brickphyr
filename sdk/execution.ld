/* SPDX-License-Identifier: Apache-2.0 */
SECTION_DATA_PROLOGUE(brickphyr_execution,(NOLOAD),)
{
	__brickphyr_execution_start = .;
	*(.brickphyr_execution)
	__brickphyr_execution_end = .;
} GROUP_DATA_LINK_IN(RAMABLE_REGION, RAMABLE_REGION)
