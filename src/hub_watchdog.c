// SPDX-License-Identifier: Apache-2.0
#include "hub_watchdog.h"
#include "hub_reset.h"
#include <inttypes.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/task_wdt/task_wdt.h>

LOG_MODULE_REGISTER(hub_watchdog);

static int hub_channel = -1;
static int work_channel = -1;
static int status = -EAGAIN;
static k_tid_t hub_thread;

static void work_progress(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(progress, work_progress);

static void work_progress(struct k_work *work)
{
	task_wdt_feed(work_channel);
	k_work_schedule(&progress, K_MSEC(CONFIG_BRICKPHYR_WATCHDOG_TIMEOUT_MS / 4));
}

void hub_watchdog_feed(void)
{
	if (!status && k_current_get() == hub_thread) {
		task_wdt_feed(hub_channel);
	}
}

int hub_watchdog_run(void)
{
	if (status) {
		return status;
	}
	hub_thread = k_current_get();
	/* Initialization can include filesystem recovery; normal progress has a
	 * shorter deadline. No hardware watchdog is stopped during this handover. */
	task_wdt_delete(hub_channel);
	hub_channel = task_wdt_add(CONFIG_BRICKPHYR_WATCHDOG_TIMEOUT_MS, NULL, NULL);
	if (hub_channel < 0) {
		status = hub_channel;
		return status;
	}
	work_channel = task_wdt_add(CONFIG_BRICKPHYR_WATCHDOG_TIMEOUT_MS, NULL, NULL);
	if (work_channel < 0) {
		status = work_channel;
		return status;
	}
	k_work_schedule(&progress, K_NO_WAIT);
	return 0;
}

static int initialize(void)
{
	const struct device *wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

	status = device_is_ready(wdt) ? task_wdt_init(wdt) : -ENODEV;
	if (!status) {
		hub_channel = task_wdt_add(CONFIG_BRICKPHYR_WATCHDOG_BOOT_TIMEOUT_MS, NULL, NULL);
		status = hub_channel < 0 ? hub_channel : 0;
	}
	if (status) {
		LOG_ERR("Watchdog startup failed: %d", status);
	}
	return status;
}

/* IWDG is initialized at device priority 50. Arm before SPI-NOR and application
 * storage initialization; clocks, early devices and the bootloader precede it. */
SYS_INIT(initialize, POST_KERNEL, 51);

static int cmd_watchdog(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t reset_cause;
	int reset_error = hub_reset_cause(&reset_cause);
	shell_print(shell, "Watchdog: %s; error %d; progress timeout %u ms",
		status ? "unavailable" : "armed", status, CONFIG_BRICKPHYR_WATCHDOG_TIMEOUT_MS);
	if (reset_error) {
		shell_error(shell, "Reset cause read/clear failed: %d", reset_error);
		return reset_error;
	}
	shell_print(shell, "Reset cause: 0x%08" PRIx32 "%s%s", reset_cause,
		reset_cause & RESET_WATCHDOG ? " watchdog" : "",
		reset_cause & RESET_SOFTWARE ? " software" : "");
	return status;
}

SHELL_CMD_ARG_REGISTER(watchdog, NULL, "System watchdog and reset cause.", cmd_watchdog, 1, 0);
