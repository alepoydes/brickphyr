// SPDX-License-Identifier: Apache-2.0
#include "configuration.h"
#include "storage.h"
#ifdef CONFIG_BRICKPHYR_PORT_UART
#include "uart_settings.h"
#endif

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

int config_index(const struct config_entry *entries, size_t count, const char *name)
{
	if (name) {
		for (size_t i = 0; i < count; i++) {
			const char *suffix = strchr(entries[i].name, '/') + 1;
			if (!strcmp(name, suffix)) {
				return i;
			}
		}
	}
	return -ENOENT;
}

int config_load(const struct config_entry *entries, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		uint8_t value[CONFIG_VALUE_BYTES];
		size_t size = entries[i].count * sizeof(uint32_t);
		int length = settings_load_one(entries[i].name, value, sizeof(value));
		if (length == 0 || length == -ENOENT) {
			continue;
		}
		if (length != size) {
			return length < 0 ? length : -EINVAL;
		}
		int err = settings_runtime_set(entries[i].name, value, size);
		if (err) {
			return err;
		}
	}
	return 0;
}

int config_export(const struct config_entry *entries, size_t count,
	int (*save)(const char *, const void *, size_t))
{
	for (size_t i = 0; i < count; i++) {
		uint8_t value[CONFIG_VALUE_BYTES];
		size_t size = entries[i].count * sizeof(uint32_t);
		int err = settings_runtime_get(entries[i].name, value, sizeof(value));
		if (err != size) {
			return err < 0 ? err : -EINVAL;
		}
		err = save(entries[i].name, value, size);
		if (err) {
			return err;
		}
	}
	return 0;
}

static const struct config_entry *find_entry(const char *name)
{
	const struct { const struct config_entry *entries; size_t count; } groups[] = {
		{power_config, ARRAY_SIZE(power_config)},
#ifdef CONFIG_BRICKPHYR_PORT_UART
		{uart_config, ARRAY_SIZE(uart_config)},
#endif
#ifdef CONFIG_BRICKPHYR_IMU
		{imu_config, ARRAY_SIZE(imu_config)},
#endif
	};
	for (size_t i = 0; i < ARRAY_SIZE(groups); i++) {
		for (size_t j = 0; j < groups[i].count; j++) {
			if (!strcmp(name, groups[i].entries[j].name)) {
				return &groups[i].entries[j];
			}
		}
	}
	return NULL;
}

static void print_value(const struct shell *shell, const struct config_entry *entry,
	const uint8_t *value, const char *suffix)
{
	for (size_t i = 0; i < entry->count; i++) {
		uint32_t number = sys_get_le32(value + i * sizeof(uint32_t));
		if (entry->is_signed) {
			shell_fprintf(shell, SHELL_NORMAL, "%s%" PRId32, i ? " " : "", (int32_t)number);
		} else {
			shell_fprintf(shell, SHELL_NORMAL, "%s%" PRIu32, i ? " " : "", number);
		}
	}
	shell_print(shell, " %s (%s)", entry->unit, suffix);
}

static int get(const struct shell *shell, const struct config_entry *entry)
{
	uint8_t value[CONFIG_VALUE_BYTES], saved[CONFIG_VALUE_BYTES];
	size_t size = entry->count * sizeof(uint32_t);
	int length = settings_runtime_get(entry->name, value, sizeof(value));
	if (length != size) {
		return length < 0 ? length : -EINVAL;
	}
	shell_fprintf(shell, SHELL_NORMAL, "%s = ", entry->name);
	print_value(shell, entry, value, "selected and effective");
	if (!IS_ENABLED(CONFIG_SETTINGS_FILE)) {
		shell_print(shell, "RAM only; persistent storage unavailable.");
		return 0;
	}
	int status = storage_status();
	length = status ? status : settings_load_one(entry->name, saved, sizeof(saved));
	if (length == size) {
		shell_fprintf(shell, SHELL_NORMAL, "Saved: ");
		print_value(shell, entry, saved, memcmp(value, saved, size) ? "RAM differs" : "unchanged");
	} else if (length == 0 || length == -ENOENT) {
		shell_print(shell, "Not saved.");
	} else {
		return length < 0 ? length : -EINVAL;
	}
	return 0;
}

static int cmd_config(const struct shell *shell, size_t argc, char **argv)
{
	int err;
	if (argc == 2 && !strcmp(argv[1], "save")) {
		err = config_save();
		if (!err) {
			shell_print(shell, "Settings saved.");
		}
	} else if (argc >= 3) {
		const struct config_entry *entry = find_entry(argv[2]);
		if (!entry) {
			err = -ENOENT;
			goto out;
		}
		if (argc == 3 && !strcmp(argv[1], "get")) {
			err = get(shell, entry);
		} else if (argc == 3 + entry->count && !strcmp(argv[1], "set")) {
			uint8_t value[CONFIG_VALUE_BYTES];
			for (size_t i = 0; i < entry->count; i++) {
				err = 0;
				int64_t number = entry->is_signed ? shell_strtol(argv[i + 3], 10, &err) :
					(int64_t)shell_strtoul(argv[i + 3], 10, &err);
				if (err || (!entry->is_signed && argv[i + 3][0] == '-') ||
				    number < (entry->is_signed ? INT32_MIN : 0) ||
				    number > (entry->is_signed ? (int64_t)INT32_MAX : UINT32_MAX)) {
					shell_error(shell, "Use %s 32-bit integer values in %s.",
						entry->is_signed ? "signed" : "unsigned", entry->unit);
					return -EINVAL;
				}
				sys_put_le32((uint32_t)number, value + i * sizeof(uint32_t));
			}
			err = settings_runtime_set(entry->name, value, entry->count * sizeof(uint32_t));
			if (!err) {
				shell_print(shell, "Applied in RAM. Use config save to persist changes.");
			}
		} else {
			err = -EINVAL;
		}
	} else {
		shell_error(shell, "Usage: config get NAME | set NAME VALUE... | save");
		return -EINVAL;
	}
out:
	if (err) {
		shell_error(shell, "Setting operation failed: %d", err);
	}
	return err;
}

SHELL_CMD_ARG_REGISTER(config, NULL, "Read/change runtime settings; save explicitly.",
		      cmd_config, 2, 7);

int config_save(void)
{
	int err = IS_ENABLED(CONFIG_SETTINGS_FILE) ? storage_status() : -ENOTSUP;
	return err ? err : settings_save();
}
