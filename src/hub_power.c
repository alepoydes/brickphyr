// SPDX-License-Identifier: Apache-2.0
#include "hub_power.h"
#include "hub_battery.h"
#include "configuration.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

/* Settings are uint32 little-endian seconds. Zero disables automatic shutdown.
 * Defaults preserve the bring-up firmware's indefinite power retention. */
const struct config_entry power_config[] = {
	{"power/battery_idle_s", "s", 1, false},
	{"power/usb_idle_s", "s", 1, false},
};
static uint32_t timeout_s[ARRAY_SIZE(power_config)];
static struct k_spinlock lock;
static int source = -ENODEV;
static enum hub_connection connected;
static enum hub_connection link;
static bool application;
static bool shutting_down;
static int64_t idle_since;
static int64_t sampled_ms;
static int64_t critical_since = -1;

void hub_power_get(struct hub_power_state *state)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*state = (struct hub_power_state){
		.sampled_ms = sampled_ms, .idle_since_ms = idle_since, .source = source,
		.battery_idle_s = timeout_s[0], .usb_idle_s = timeout_s[1],
		.connected = connected, .application = application, .shutting_down = shutting_down,
		.transport = link,
	};
	k_spin_unlock(&lock, key);
}

static int setting_get(const char *name, char *value, int capacity)
{
	int index = config_index(power_config, ARRAY_SIZE(power_config), name);
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
	int index = config_index(power_config, ARRAY_SIZE(power_config), name);
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

static int setting_export(int (*save)(const char *, const void *, size_t))
{
	return config_export(power_config, ARRAY_SIZE(power_config), save);
}

SETTINGS_STATIC_HANDLER_DEFINE(power, "power", setting_get, setting_set, NULL, setting_export);

int hub_power_load(void)
{
	return config_load(power_config, ARRAY_SIZE(power_config));
}

bool hub_power_connection(enum hub_connection open)
{
	if (open < HUB_DISCONNECTED || open > HUB_BLE) return false;
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool allowed = !open || !shutting_down;
	if (allowed && connected != open) {
		connected = open;
		idle_since = k_uptime_get();
	}
	if (allowed) link = open;
	k_spin_unlock(&lock, key);
	return allowed;
}

void hub_power_link_closed(enum hub_connection transport)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (link == transport) link = HUB_DISCONNECTED;
	k_spin_unlock(&lock, key);
}

void hub_power_request_shutdown(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	shutting_down = true;
	k_spin_unlock(&lock, key);
}

bool hub_power_application(bool running)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool allowed = !running || !shutting_down;
	if (allowed && application != running) {
		application = running;
		idle_since = k_uptime_get();
	}
	k_spin_unlock(&lock, key);
	return allowed;
}

bool hub_power_update(int vbus, bool activity, int64_t now)
{
	struct bp_battery_sample battery;
	bool critical = hub_battery_get(&battery) == 0 && battery.critical;
	k_spinlock_key_t key = k_spin_lock(&lock);
	sampled_ms = now;
	if (source != vbus || activity) {
		source = vbus;
		idle_since = now;
	}
	if (!connected && !application && source >= 0 && source <= 1 && timeout_s[source] &&
	    now - idle_since >= (int64_t)timeout_s[source] * 1000) {
		shutting_down = true;
	}
	/* Reference Pybricks battery protection: three seconds continuously below
	 * the hysteretic critical threshold. An app or host cannot inhibit it.
	 * Read failures retain a previously established critical condition. */
	if (!critical) critical_since = -1;
	else if (critical_since < 0) critical_since = now;
	else if (now - critical_since >= 3000) shutting_down = true;
	bool expired = shutting_down;
	k_spin_unlock(&lock, key);
	return expired;
}

static int cmd_power(const struct shell *shell, size_t argc, char **argv)
{
	struct hub_power_state state;
	hub_power_get(&state);
	int vbus = state.source;
	bool owner = state.connected;
	bool running = state.application;
	bool stopping = state.shutting_down;
	uint32_t battery = state.battery_idle_s;
	uint32_t usb = state.usb_idle_s;
	uint32_t seconds = vbus == 0 ? battery : usb;
	int64_t remaining = MAX((int64_t)seconds * 1000 - (k_uptime_get() - state.idle_since_ms), 0);
	shell_print(shell, "Source: %s; management owner: %s",
		vbus < 0 ? "unknown" : vbus ? "USB" : "battery", owner ? "active" : "none");
	shell_print(shell, "Idle timeout: battery %" PRIu32 " s, USB %" PRIu32 " s",
		battery, usb);
	const char *inhibit = stopping ? "shutdown pending" : owner ? "active management session" :
		running ? "running application" :
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

SHELL_CMD_ARG_REGISTER(power, NULL, "Power source, owner, and inactivity policy.", cmd_power, 1, 0);
