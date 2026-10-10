// SPDX-License-Identifier: Apache-2.0

#include <inttypes.h>
#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <zephyr/version.h>

static int cmd_info(const struct shell *shell, size_t argc, char **argv)
{
	shell_print(shell, "Brickphyr %s (%s)", APP_VERSION_STRING, STRINGIFY(APP_BUILD_VERSION));
	shell_print(shell, "Zephyr %s", KERNEL_VERSION_STRING);
	shell_print(shell, "Target %s", CONFIG_BOARD_TARGET);
	return 0;
}

static int cmd_uptime(const struct shell *shell, size_t argc, char **argv)
{
	shell_print(shell, "Uptime: %" PRId64 " ms", k_uptime_get());
	return 0;
}

static int cmd_reboot(const struct shell *shell, size_t argc, char **argv)
{
	/* Do not wait for a terminal to drain output before resetting the board. */
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

SHELL_CMD_ARG_REGISTER(info, NULL, "Firmware version and build target.", cmd_info, 1, 0);
SHELL_CMD_ARG_REGISTER(uptime, NULL, "Monotonic time since boot, in milliseconds.", cmd_uptime, 1, 0);
SHELL_CMD_ARG_REGISTER(reboot, NULL, "Restart the firmware.", cmd_reboot, 1, 0);

int main(void)
{
	/* Shell and USB run in their own threads; boot never waits for a host. */
	return 0;
}
