// SPDX-License-Identifier: Apache-2.0
#include "hub_sound.h"
#include "hub_ports.h"

#include <inttypes.h>
#include <string.h>
#include <errno.h>
#ifdef CONFIG_BRICKPHYR_NATIVE
#include "native.h"
#endif
#ifdef CONFIG_BOARD_SPIKE_PRIME_F4
#include "hub.h"
#endif
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
	if (argc == 2) {
		if (strcmp(argv[1], "dfu")) {
			shell_error(shell, "Usage: reboot [dfu]");
			return -EINVAL;
		}
#ifdef CONFIG_BOARD_SPIKE_PRIME_F4
		int err = hub_reboot_dfu();
		shell_error(shell, "DFU entry failed: %d", err);
		return err;
#else
		shell_error(shell, "DFU is not supported on this target.");
		return -ENOTSUP;
#endif
	}
	/* Do not wait for a terminal to drain output before resetting the board. */
#ifdef CONFIG_BRICKPHYR_NATIVE
	int err = native_shutdown();
	if (err) {
		return err;
	}
#endif
	int sound_error = hub_sound_shutdown();
	if (sound_error) {
		return sound_error;
	}
	int port_error = hub_ports_shutdown();
	if (port_error) {
		return port_error;
	}
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

SHELL_CMD_ARG_REGISTER(info, NULL, "Firmware version and build target.", cmd_info, 1, 0);
SHELL_CMD_ARG_REGISTER(uptime, NULL, "Monotonic time since boot, in milliseconds.", cmd_uptime, 1, 0);
SHELL_CMD_ARG_REGISTER(reboot, NULL, "Restart the firmware, or enter the existing DFU loader: reboot [dfu].", cmd_reboot, 1, 1);

int main(void)
{
#ifdef CONFIG_BOARD_SPIKE_PRIME_F4
	hub_run();
#endif
	/* Shell and USB run in their own threads; boot never waits for a host. */
	return 0;
}
