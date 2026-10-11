// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>

BRICKPHYR_APPLICATION();

int app_main(void)
{
	uint32_t count = 0;
	while (1) {
		char line[] = "counter 00000000 buttons 0\n";
		const char hex[] = "0123456789abcdef";
		for (unsigned int i = 0; i < 8; i++) {
			line[8 + i] = hex[(count >> ((7 - i) * 4)) & 15];
		}
		line[25] = hex[bp_buttons() & 15];
		bp_write(line, sizeof(line) - 1);
		count++;
		bp_sleep(1000);
	}
}
