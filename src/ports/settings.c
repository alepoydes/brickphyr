// SPDX-License-Identifier: Apache-2.0
#include "uart_settings.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

const struct config_entry uart_config[1] = {
	{"uart/power_allowed", "boolean", 1, false},
};

static struct k_spinlock lock;
static bool power_allowed;

bool hub_uart_power_allowed(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool allowed = power_allowed;
	k_spin_unlock(&lock, key);
	return allowed;
}

int hub_uart_allow_power(uint32_t allowed)
{
	if (allowed > 1) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&lock);
	power_allowed = allowed;
	k_spin_unlock(&lock, key);
	return 0;
}

static int setting_get(const char *name, char *value, int capacity)
{
	int index = config_index(uart_config, ARRAY_SIZE(uart_config), name);
	if (index < 0 || capacity < (int)sizeof(uint32_t)) return index < 0 ? index : -ENOSPC;
	sys_put_le32(hub_uart_power_allowed(), (uint8_t *)value);
	return sizeof(uint32_t);
}

static int setting_set(const char *name, size_t length, settings_read_cb read, void *context)
{
	int index = config_index(uart_config, ARRAY_SIZE(uart_config), name);
	if (index < 0 || length != sizeof(uint32_t)) return index < 0 ? index : -EINVAL;
	uint8_t value[sizeof(uint32_t)];
	ssize_t count = read(context, value, sizeof(value));
	if (count != sizeof(value)) return count < 0 ? count : -EINVAL;
	return hub_uart_allow_power(sys_get_le32(value));
}

static int setting_export(int (*save)(const char *, const void *, size_t))
{
	return config_export(uart_config, ARRAY_SIZE(uart_config), save);
}

SETTINGS_STATIC_HANDLER_DEFINE(uart, "uart", setting_get, setting_set, NULL, setting_export);
