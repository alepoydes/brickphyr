// SPDX-License-Identifier: Apache-2.0
#include "native.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/fff.h>

DEFINE_FFF_GLOBALS;

uint32_t hub_buttons(void) { return 5; } /* Synthetic buttons. */
int storage_status(void) { return -ENOTSUP; }

int main(void)
{
	for (;;) {
		native_poll();
		k_msleep(20);
	}
}
