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
#include "hub_power.h"

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
	const struct flash_area *area;
	status = flash_area_open(PARTITION_ID(settings_partition), &area);
	if (status) {
		return status;
	}
	/* Only an entirely erased partition may be formatted automatically. A failed
	 * mount of existing data must never erase another firmware's user storage. */
	uint8_t bytes[256];
	bool empty = true;
	for (size_t offset = 0; empty && offset < area->fa_size; offset += sizeof(bytes)) {
		size_t length = MIN(sizeof(bytes), area->fa_size - offset);
		status = flash_area_read(area, offset, bytes, length);
		if (status) {
			break;
		}
		for (size_t i = 0; i < length; i++) {
			if (bytes[i] != flash_area_erased_val(area)) {
				empty = false;
				break;
			}
		}
	}
	flash_area_close(area);
	if (!status && empty) {
		status = fs_mkfs(FS_LITTLEFS, (uintptr_t)mount.storage_dev, &filesystem, 0);
	}
	if (!status) {
		status = fs_mount(&mount);
	}
	if (!status) {
		status = settings_subsys_init();
	}
	if (!status) {
		status = hub_power_load();
	}
	if (status) {
		LOG_ERR("Settings storage unavailable: %d; existing data preserved", status);
	}
	return status;
}

SYS_INIT(storage_init, APPLICATION, 80);

static int cmd_storage(const struct shell *shell, size_t argc, char **argv)
{
	bool read = !strcmp(argv[1], "read");
	if (!read && strcmp(argv[1], "info")) {
		return -EINVAL;
	}
	const struct flash_area *area;
	int err = flash_area_open(PARTITION_ID(settings_partition), &area);
	if (err) {
		shell_error(shell, "Open settings partition failed: %d", err);
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
			shell_print(shell, "Settings: offset 0x%08lx, size %lu bytes",
				(long)area->fa_off, (unsigned long)area->fa_size);
			shell_print(shell, "Settings initialization: %d", status);
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

SHELL_CMD_ARG_REGISTER(storage, NULL, "storage info | read: inspect the settings partition.",
		      cmd_storage, 2, 0);
