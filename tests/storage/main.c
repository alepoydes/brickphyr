// SPDX-License-Identifier: Apache-2.0
#include "storage.h"
#include "hub_power.h"

#include <zephyr/fs/fs.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

ZTEST(storage, test_persistence_or_foreign_partition)
{
	/* Python supplies either erased flash or foreign bytes before firmware boot. */
	if (storage_status()) {
		zassert_equal(storage_status(), -EFAULT);
		printk("FOREIGN STORAGE REJECTED\n");
		return;
	}
	uint8_t desired[4];
	uint8_t effective[4];
	sys_put_le32(123, desired);
	zassert_ok(settings_runtime_set("power/battery_idle_s", desired, sizeof(desired)));
	zassert_ok(settings_save());
	/* Cross the Settings file compaction threshold through the actual exporter. */
	for (uint32_t i = 0; i < CONFIG_SETTINGS_FILE_MAX_LINES + 2; i++) {
		sys_put_le32(200 + i, desired);
		zassert_ok(settings_runtime_set("power/battery_idle_s", desired, sizeof(desired)));
		zassert_ok(settings_save());
	}
	sys_put_le32(123, desired);
	zassert_ok(settings_runtime_set("power/battery_idle_s", desired, sizeof(desired)));
	zassert_ok(settings_save());
	sys_put_le32(456, desired);
	zassert_ok(settings_runtime_set("power/battery_idle_s", desired, sizeof(desired)));
	zassert_equal(settings_load_one("power/battery_idle_s", effective, sizeof(effective)), 4);
	zassert_equal(sys_get_le32(effective), 123, "RAM mutation implicitly saved");
	zassert_ok(hub_power_load());
	zassert_equal(settings_runtime_get("power/battery_idle_s", effective, sizeof(effective)), 4);
	zassert_equal(sys_get_le32(effective), 123);
	/* An incompatible persisted value must be diagnosed without applying it. */
	zassert_ok(settings_save_one("power/battery_idle_s", desired, 3));
	zassert_equal(hub_power_load(), -EINVAL);
	zassert_equal(settings_runtime_get("power/battery_idle_s", effective, sizeof(effective)), 4);
	zassert_equal(sys_get_le32(effective), 123);
	zassert_ok(settings_save());
	/* Truncated framing must not turn a read failure into successful cleanup. */
	struct fs_file_t file;
	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, CONFIG_SETTINGS_FILE_PATH, FS_O_WRITE | FS_O_APPEND));
	const uint8_t partial_header = 7;
	zassert_equal(fs_write(&file, &partial_header, 1), 1);
	zassert_ok(fs_close(&file));
	zassert_equal(settings_load_one("power/missing", effective, sizeof(effective)), -ESPIPE);
	zassert_ok(fs_unlink(CONFIG_SETTINGS_FILE_PATH));
	zassert_ok(settings_save_one("power/battery_idle_s", desired, sizeof(desired)));
	struct fs_dirent entry;
	zassert_ok(fs_stat(CONFIG_SETTINGS_FILE_PATH, &entry));
	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, CONFIG_SETTINGS_FILE_PATH, FS_O_WRITE));
	zassert_ok(fs_truncate(&file, entry.size - 2));
	zassert_ok(fs_close(&file));
	zassert_equal(hub_power_load(), -ESPIPE);
	printk("SETTINGS SAVE AND RELOAD VERIFIED\n");
}

ZTEST_SUITE(storage, NULL, NULL, NULL, NULL, NULL);
