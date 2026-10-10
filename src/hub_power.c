// SPDX-License-Identifier: Apache-2.0
#include "hub_power.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

/* Settings are uint32 little-endian seconds. Zero disables automatic shutdown.
 * Defaults preserve the bring-up firmware's indefinite power retention. */
static const char *const names[] = {"battery_idle_s", "usb_idle_s"};
static uint32_t timeout_s[ARRAY_SIZE(names)];
static struct k_spinlock lock;
static int source = -ENODEV;
static bool connected;
static bool shutting_down;
static int64_t idle_since;

static int setting_index(const char *name)
{
	if (!name) {
		return -ENOENT;
	}
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		if (!strcmp(name, names[i])) {
			return i;
		}
	}
	return -ENOENT;
}

static int setting_get(const char *name, char *value, int capacity)
{
	int index = setting_index(name);
	if (index < 0 || capacity < (int)sizeof(uint32_t)) {
		return index < 0 ? index : -ENOSPC;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	sys_put_le32(timeout_s[index], (uint8_t *)value);
	k_spin_unlock(&lock, key);
	return sizeof(uint32_t);
}

static int setting_set(const char *name, size_t length, settings_read_cb read, void *context)
{
	int index = setting_index(name);
	if (index < 0 || length != sizeof(uint32_t)) {
		return index < 0 ? index : -EINVAL;
	}
	uint8_t value[sizeof(uint32_t)];
	ssize_t count = read(context, value, sizeof(value));
	if (count != sizeof(value)) {
		return count < 0 ? count : -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t seconds = sys_get_le32(value);
	if (timeout_s[index] != seconds) {
		timeout_s[index] = seconds;
		idle_since = k_uptime_get();
	}
	k_spin_unlock(&lock, key);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(power, "power", setting_get, setting_set, NULL, NULL);

bool hub_power_connection(bool open)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool allowed = !open || !shutting_down;
	if (allowed && connected != open) {
		connected = open;
		idle_since = k_uptime_get();
	}
	k_spin_unlock(&lock, key);
	return allowed;
}

bool hub_power_update(int vbus, bool activity, int64_t now)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (source != vbus || activity) {
		source = vbus;
		idle_since = now;
	}
	if (!connected && source >= 0 && source <= 1 && timeout_s[source] &&
	    now - idle_since >= (int64_t)timeout_s[source] * 1000) {
		shutting_down = true;
	}
	bool expired = shutting_down;
	k_spin_unlock(&lock, key);
	return expired;
}

static int cmd_power(const struct shell *shell, size_t argc, char **argv)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int vbus = source;
	bool owner = connected;
	bool stopping = shutting_down;
	uint32_t battery = timeout_s[0];
	uint32_t usb = timeout_s[1];
	uint32_t seconds = vbus == 0 ? battery : usb;
	int64_t remaining = MAX((int64_t)seconds * 1000 - (k_uptime_get() - idle_since), 0);
	k_spin_unlock(&lock, key);
	shell_print(shell, "Source: %s; management owner: %s",
		vbus < 0 ? "unknown" : vbus ? "USB" : "battery", owner ? "USB" : "none");
	shell_print(shell, "Idle timeout: battery %" PRIu32 " s, USB %" PRIu32 " s",
		battery, usb);
	const char *inhibit = stopping ? "shutdown pending" : owner ? "active management session" :
		vbus < 0 ? "power source unavailable" : !seconds ? "timeout disabled" : NULL;
	if (inhibit) {
		shell_print(shell, "Automatic shutdown: %s", inhibit);
	} else {
		shell_print(shell, "Automatic shutdown in %" PRId64 " ms", remaining);
	}
	if (vbus < 0) {
		shell_error(shell, "VBUS read failed: %d", vbus);
	}
	return 0;
}

static int cmd_config(const struct shell *shell, size_t argc, char **argv)
{
	uint8_t value[sizeof(uint32_t)];
	if (argc == 2 && !strcmp(argv[1], "save")) {
		shell_error(shell, "Persistent settings storage is not available.");
		return -ENOTSUP;
	}
	int err;
	if (argc == 3 && !strcmp(argv[1], "get")) {
		err = settings_runtime_get(argv[2], value, sizeof(value));
		if (err == sizeof(value)) {
			shell_print(shell, "%s = %" PRIu32 " s (effective; RAM only, not saved)",
				argv[2], sys_get_le32(value));
			return 0;
		}
	} else if (argc == 4 && !strcmp(argv[1], "set")) {
		err = 0;
		unsigned long seconds = shell_strtoul(argv[3], 10, &err);
		if (err || argv[3][0] == '-' || seconds > UINT32_MAX) {
			shell_error(shell, "Use integer seconds, 0 through 4294967295; 0 disables.");
			return -EINVAL;
		}
		sys_put_le32(seconds, value);
		err = settings_runtime_set(argv[2], value, sizeof(value));
		if (!err) {
			shell_print(shell, "Applied in RAM; not saved.");
			return 0;
		}
	} else {
		shell_error(shell, "Usage: config get NAME | set NAME SECONDS | save");
		return -EINVAL;
	}
	shell_error(shell, "Setting operation failed: %d", err);
	return err;
}

SHELL_CMD_ARG_REGISTER(power, NULL, "Power source, owner, and inactivity policy.", cmd_power, 1, 0);
SHELL_CMD_ARG_REGISTER(config, NULL, "Runtime settings: power/battery_idle_s, power/usb_idle_s.",
		      cmd_config, 2, 2);
