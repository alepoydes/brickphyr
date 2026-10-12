// SPDX-License-Identifier: Apache-2.0
#include "hub_reset.h"
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>

static uint32_t reset_cause;
static int reset_error = -EAGAIN;

int hub_reset_cause(uint32_t *cause)
{
	*cause = reset_cause;
	return reset_error;
}

static int capture(void)
{
	reset_error = hwinfo_get_reset_cause(&reset_cause);
	if (!reset_error) reset_error = hwinfo_clear_reset_cause();
	return 0;
}
SYS_INIT(capture, POST_KERNEL, 51);
