// SPDX-License-Identifier: Apache-2.0
#include "device.h"
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/ztest.h>

unsigned int watchdog_feeds, watchdog_starts, watchdog_timeout;

static int install(const struct device *dev, const struct wdt_timeout_cfg *config)
{
	zassert_equal(config->flags, WDT_FLAG_RESET_SOC);
	zassert_equal(config->window.min, 0);
	watchdog_timeout = config->window.max;
	return 0;
}

static int setup(const struct device *dev, uint8_t options)
{
	zassert_false(options & WDT_OPT_PAUSE_IN_SLEEP);
	watchdog_starts++;
	return 0;
}

static int feed(const struct device *dev, int channel)
{
	zassert_equal(watchdog_starts, 1);
	watchdog_feeds++;
	return 0;
}

static DEVICE_API(wdt, api) = {.install_timeout = install, .setup = setup, .feed = feed};
DEVICE_DT_DEFINE(DT_NODELABEL(test_watchdog), NULL, NULL, NULL, NULL,
		 POST_KERNEL, 50, &api);
