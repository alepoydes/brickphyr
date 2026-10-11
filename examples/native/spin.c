// SPDX-License-Identifier: Apache-2.0
/* Diagnostic: explicit STOP must terminate a program that never yields. */
#include <brickphyr.h>
BRICKPHYR_APPLICATION();

int app_main(void)
{
	while (1) {
		__asm__ volatile ("nop");
	}
}
