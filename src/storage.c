// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/settings/settings.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include "storage.h"
#include "storage_mount.h"
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
#include "app_store.h"
#include "app_data.h"
#endif
#include "hub_power.h"
#include "configuration.h"
#ifdef CONFIG_BRICKPHYR_PORT_UART
#include "uart_settings.h"
#endif

LOG_MODULE_REGISTER(storage);

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(filesystem);
static struct fs_mount_t mount = {
	.type = FS_LITTLEFS,
	.fs_data = &filesystem,
	.storage_dev = (void *)PARTITION_ID(settings_partition),
	.mnt_point = "/settings",
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};
static int status = -EAGAIN;

int storage_status(void)
{
	return status;
}

static int storage_init(void)
{
	status = storage_mount_preserved(&mount);
	if (!status) {
		status = settings_subsys_init();
	}
	if (!status) {
		status = hub_power_load();
	}
#ifdef CONFIG_BRICKPHYR_PORT_UART
	if (!status) {
		status = config_load(uart_config, ARRAY_SIZE(uart_config));
	}
#endif
#ifdef CONFIG_BRICKPHYR_IMU
	if (!status) {
		status = config_load(imu_config, ARRAY_SIZE(imu_config));
	}
#endif
	if (status) {
		LOG_ERR("Settings storage unavailable: %d; existing data preserved", status);
	}
	return status;
}

SYS_INIT(storage_init, APPLICATION, 80);

static int cmd_storage(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "save") && !strcmp(argv[2], "data")) {
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
		int err = app_data_save();
#else
		int err = -ENOTSUP;
#endif
		shell_print(shell, "Save user data: %d", err);
		return err;
	}
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
	if (argc == 3 && !strcmp(argv[1], "format") && !strcmp(argv[2], "apps")) {
		int err = app_store_format();
		shell_print(shell, "Format apps: %d", err);
		return err;
	}
#endif

	bool read = !strcmp(argv[1], "read");
	if (!read && strcmp(argv[1], "info")) {
		return -EINVAL;
	}
	const char *name = argc == 3 ? argv[2] : "settings";
	uint8_t partition = PARTITION_ID(settings_partition);
	if (strcmp(name, "settings")) {
#if DT_NODE_EXISTS(DT_NODELABEL(apps_partition))
		if (strcmp(name, "apps")) {
			return -EINVAL;
		}
		partition = PARTITION_ID(apps_partition);
#else
		return -ENOTSUP;
#endif
	}
	const struct flash_area *area;
	int err = flash_area_open(partition, &area);
	if (err) {
		shell_error(shell, "Open %s partition failed: %d", name, err);
		return err;
	}
	if (!device_is_ready(area->fa_dev)) {
		err = -ENODEV;
		goto out;
	}
	if (!read) {
		uint8_t id[3];
		if (IS_ENABLED(CONFIG_FLASH_JESD216_API)) {
			err = flash_read_jedec_id(area->fa_dev, id);
			if (!err) {
				shell_print(shell, "JEDEC: %02x %02x %02x", id[0], id[1], id[2]);
			}
		}
		if (!err) {
			shell_print(shell, "%s: offset 0x%08lx, size %lu bytes", name,
				(long)area->fa_off, (unsigned long)area->fa_size);
			if (partition == PARTITION_ID(settings_partition)) {
				shell_print(shell, "Settings initialization: %d", status);
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
			} else {
				shell_print(shell, "Applications initialization: %d", app_store_status());
#endif
			}
		}
	} else {
		uint8_t bytes[64];
		char hex[sizeof(bytes) * 2 + 1];
		for (size_t offset = 0; offset < area->fa_size; offset += sizeof(bytes)) {
			size_t length = MIN(sizeof(bytes), area->fa_size - offset);
			err = flash_area_read(area, offset, bytes, length);
			if (err) {
				shell_error(shell, "Read at 0x%lx failed: %d", (unsigned long)offset, err);
				break;
			}
			bin2hex(bytes, length, hex, sizeof(hex));
			shell_print(shell, "%08lx: %s", (unsigned long)offset, hex);
		}
	}
out:
	flash_area_close(area);
	if (err) {
		shell_error(shell, "Storage operation failed: %d", err);
	}
	return err;
}

SHELL_CMD_ARG_REGISTER(storage, NULL, "storage info|read [settings|apps]; save data; format apps: initialize unrecognized storage.",
		      cmd_storage, 2, 1);
