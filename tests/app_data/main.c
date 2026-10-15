// SPDX-License-Identifier: Apache-2.0
#include "app_data.h"
#include "storage_mount.h"
#include <brickphyr_system.h>
#include <zephyr/drivers/flash/flash_simulator.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(filesystem);
static struct fs_mount_t mount = {
	.type = FS_LITTLEFS, .fs_data = &filesystem,
	.storage_dev = (void *)PARTITION_ID(apps_partition), .mnt_point = "/apps",
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};

static int write_failure(const struct device *dev, off_t offset, uint8_t byte) { return -EIO; }
static bool changed;
static int write_during_save(const struct device *dev, off_t offset, uint8_t byte)
{
	if (!changed) {
		const uint8_t next = 0xa6;
		zassert_ok(app_data_write(1, &next, 1));
		zassert_equal(app_data_save_start(), -EBUSY);
		changed = true;
	}
	return byte;
}

static void read_saved(uint8_t data[BP_USER_DATA_SIZE])
{
	struct fs_file_t file;
	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, "/apps/.userdata", FS_O_READ));
	zassert_equal(fs_read(&file, data, BP_USER_DATA_SIZE), BP_USER_DATA_SIZE);
	zassert_ok(fs_close(&file));
}

ZTEST(app_data, test_saved_snapshot_and_boot)
{
	int mounted = storage_mount_preserved(&mount);
	app_data_load(mounted);
	uint8_t data[BP_USER_DATA_SIZE];
	if (mounted) {
		zassert_equal(app_data_read(0, data, sizeof(data)), mounted);
		zassert_equal(app_data_write(0, data, sizeof(data)), mounted);
		zassert_equal(app_data_save(), mounted);
		printk("FOREIGN DATA STORE REJECTED\n");
		return;
	}
	int result = app_data_read(0, data, sizeof(data));
	if (result) {
		zassert_equal(result, -EBADMSG);
		zassert_equal(app_data_write(0, data, sizeof(data)), -EBADMSG);
		zassert_equal(app_data_save(), -EBADMSG);
		struct fs_dirent entry;
		zassert_ok(fs_stat("/apps/.userdata", &entry));
		zassert_equal(entry.size, BP_USER_DATA_SIZE - 1);
		printk("CORRUPT DATA REJECTED\n");
		return;
	}
	if (data[511] == 0x5a) {
		zassert_equal(data[0], 7, "Uncommitted RAM data survived reboot");
		zassert_equal(data[1], 0xa6, "Concurrent RAM update was lost");
		zassert_mem_equal(data + 3, "abc", 3);
		struct fs_file_t file;
		fs_file_t_init(&file);
		zassert_ok(fs_open(&file, "/apps/.userdata", FS_O_WRITE));
		zassert_ok(fs_truncate(&file, BP_USER_DATA_SIZE - 1));
		zassert_ok(fs_close(&file));
		printk("SAVED DATA RELOADED; CORRUPTION PREPARED\n");
		return;
	}
	for (size_t i = 0; i < sizeof(data); i++) zassert_equal(data[i], 0);
	zassert_ok(app_data_read(512, NULL, 0));
	zassert_ok(app_data_write(512, NULL, 0));
	zassert_equal(app_data_write(UINT32_MAX, data, 1), -EINVAL);
	zassert_equal(app_data_read(1, data, SIZE_MAX), -EINVAL);
	data[511] = 0x5a;
	zassert_ok(app_data_write(511, data + 511, 1));
	zassert_ok(app_data_write(3, "abc", 3));
	zassert_ok(app_data_save());
	const struct flash_area *area;
	zassert_ok(flash_area_open(PARTITION_ID(apps_partition), &area));
	const struct device *flash = area->fa_dev;
	flash_area_close(area);
	const struct flash_simulator_cb fail = {.write_byte = write_failure};
	flash_simulator_set_callbacks(flash, &fail);
	const uint8_t next = 7;
	zassert_ok(app_data_write(0, &next, 1));
	zassert_equal(app_data_save(), -EIO);
	flash_simulator_set_callbacks(flash, NULL);
	read_saved(data);
	zassert_equal(data[0], 0, "Failed save replaced committed data");
	zassert_equal(data[511], 0x5a);
	const struct flash_simulator_cb mutate = {.write_byte = write_during_save};
	flash_simulator_set_callbacks(flash, &mutate);
	zassert_ok(app_data_save());
	flash_simulator_set_callbacks(flash, NULL);
	zassert_true(changed);
	read_saved(data);
	zassert_equal(data[0], 7);
	zassert_equal(data[1], 0, "Concurrent write tore the saved snapshot");
	zassert_ok(app_data_save());
	read_saved(data);
	zassert_equal(data[1], 0xa6, "Concurrent update was marked saved too early");
	/* Capture at acceptance, before the worker can start. A caller may change
	 * RAM or finish immediately; the accepted snapshot must remain independent. */
	const uint8_t accepted = 0x41, later = 0x42;
	zassert_ok(app_data_write(2, &accepted, 1));
	k_sched_lock();
	int request = app_data_save_start();
	zassert_true(request > 0);
	zassert_equal(app_data_save_status(request), -EINPROGRESS);
	zassert_equal(app_data_save_start(), -EBUSY);
	zassert_ok(app_data_write(2, &later, 1));
	k_sched_unlock();
	while (app_data_save_status(request) == -EINPROGRESS) k_msleep(1);
	zassert_ok(app_data_save_status(request));
	read_saved(data);
	zassert_equal(data[2], accepted);
	/* The synchronous shutdown path must also commit the later RAM changes. */
	zassert_ok(app_data_save());
	zassert_equal(app_data_save_status(request), -ESTALE);
	read_saved(data);
	zassert_equal(data[2], later);
	flash_simulator_set_callbacks(flash, &fail);
	zassert_ok(app_data_save(), "Unchanged data must not write flash");
	flash_simulator_set_callbacks(flash, NULL);
	const uint8_t unsaved = 0x22;
	zassert_ok(app_data_write(0, &unsaved, 1));
	/* A leftover interrupted staging file must not override committed data. */
	struct fs_file_t file;
	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, "/apps/.userdata.tmp", FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));
	zassert_equal(fs_write(&file, "partial", 7), 7);
	zassert_ok(fs_close(&file));
	printk("USER DATA SAVED\n");
}

ZTEST_SUITE(app_data, NULL, NULL, NULL, NULL, NULL);
