// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

/* Only built into an explicit diagnostic image. Neither fault is retained
 * across reset; no command is installed in the normal firmware. */
static bool lock_irqs;

static void stall(struct k_work *work)
{
	if (lock_irqs) {
		(void)irq_lock();
		for (;;) {
			Z_SPIN_DELAY(10);
		}
	}
	k_sleep(K_FOREVER);
}

static K_WORK_DELAYABLE_DEFINE(fault, stall);

static int cmd_test(const struct shell *shell, size_t argc, char **argv)
{
	if (strcmp(argv[1], "service") && strcmp(argv[1], "hardware")) {
		return -EINVAL;
	}
	if (k_work_delayable_is_pending(&fault)) {
		return -EBUSY;
	}
	lock_irqs = !strcmp(argv[1], "hardware");
	/* Let command processing finish before the one-shot failure. */
	k_work_schedule(&fault, K_MSEC(100));
	return 0;
}

SHELL_CMD_ARG_REGISTER(watchdog_test, NULL, "One-shot reset check: service | hardware.",
		      cmd_test, 2, 0);
