// SPDX-License-Identifier: Apache-2.0
#include "hub_watchdog.h"
#include "hub_reset.h"
#include <zephyr/drivers/hwinfo.h>
#include "device.h"
#include <posix_board_if.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

static bool expecting_reset;
static int64_t stalled_at;

void __wrap_sys_reboot(int type)
{
	/* A process exit substitutes only for the board reset. Zephyr's real task
	 * watchdog detects the missing progress and reaches this boundary. */
	zassert_true(expecting_reset, "Reset during healthy operation");
	zassert_equal(type, SYS_REBOOT_COLD);
	zassert_between_inclusive(k_uptime_get() - stalled_at, 75, 150);
	printk("Watchdog recovery verified\n");
	posix_exit(0);
	CODE_UNREACHABLE;
}

#ifdef STALL_WORKQUEUE
static void stall(struct k_work *work) { k_sleep(K_FOREVER); }
static K_WORK_DEFINE(block, stall);
#endif

ZTEST(watchdog, test_progress_and_expiry)
{
	uint32_t boot, cleared, again;
	zassert_ok(hub_reset_cause(&boot));
	zassert_not_equal(boot, 0);
	zassert_ok(hwinfo_get_reset_cause(&cleared));
	zassert_equal(cleared, 0);
	zassert_ok(hub_reset_cause(&again));
	zassert_equal(again, boot);
	zassert_equal(watchdog_starts, 1);
	zassert_ok(hub_watchdog_run());
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(shell_execute_cmd(shell, "watchdog"));
	for (int i = 0; i < 30; i++) {
		hub_watchdog_feed();
		k_msleep(10);
	}
	zassert_true(watchdog_feeds > 30, "System work did not feed its watchdog");
	zassert_equal(watchdog_starts, 1, "Runtime handover restarted hardware");
	expecting_reset = true;
	stalled_at = k_uptime_get();
#ifdef STALL_WORKQUEUE
	k_work_submit(&block);
	for (int i = 0; i < 20; i++) {
		hub_watchdog_feed();
		k_msleep(10);
	}
#else
	k_msleep(200);
#endif
	zassert_unreachable("Missing system progress did not reset the board");
}

ZTEST_SUITE(watchdog, NULL, NULL, NULL, NULL, NULL);
