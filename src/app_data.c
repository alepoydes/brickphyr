// SPDX-License-Identifier: Apache-2.0
#include "app_data.h"
#include <brickphyr_system.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_data);
static K_MUTEX_DEFINE(save_lock);
static struct k_spinlock data_lock;
static uint8_t user_data[BP_USER_DATA_SIZE];
static bool dirty;
static int data_error = -EAGAIN;
#define DATA_PATH "/apps/.userdata"
#define DATA_STAGE "/apps/.userdata.tmp"

/* Called before applications can start, or after explicitly formatting an
 * unavailable store. A malformed file is diagnosed, never replaced by defaults. */
void app_data_load(int storage_error)
{
	uint8_t loaded[sizeof(user_data)] = {0};
	struct fs_dirent entry;
	int err = storage_error ? storage_error : fs_stat(DATA_PATH, &entry);
	bool missing = !storage_error && err == -ENOENT;
	if (missing) err = 0;
	if (!err && !missing && (entry.type != FS_DIR_ENTRY_FILE || entry.size != sizeof(loaded))) {
		err = -EBADMSG;
	}
	if (!err && !missing) {
		struct fs_file_t file;
		fs_file_t_init(&file);
		err = fs_open(&file, DATA_PATH, FS_O_READ);
		if (!err) {
			ssize_t size = fs_read(&file, loaded, sizeof(loaded));
			int closed = fs_close(&file);
			err = size < 0 ? size : size != sizeof(loaded) ? -EIO : closed;
		}
	}
	k_spinlock_key_t key = k_spin_lock(&data_lock);
	data_error = err;
	if (!err) {
		memcpy(user_data, loaded, sizeof(user_data));
		dirty = false;
	}
	k_spin_unlock(&data_lock, key);
	if (err) LOG_ERR("User data load failed: %d; existing data preserved", err);
}

int app_data_read(uint32_t offset, void *data, size_t size)
{
	if (offset > sizeof(user_data) || size > sizeof(user_data) - offset) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&data_lock);
	int err = data_error;
	if (!err && size) memcpy(data, user_data + offset, size);
	k_spin_unlock(&data_lock, key);
	return err;
}

int app_data_write(uint32_t offset, const void *data, size_t size)
{
	if (offset > sizeof(user_data) || size > sizeof(user_data) - offset) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&data_lock);
	int err = data_error;
	if (!err && size && memcmp(user_data + offset, data, size)) {
		memcpy(user_data + offset, data, size);
		dirty = true;
	}
	k_spin_unlock(&data_lock, key);
	return err;
}

int app_data_save(void)
{
	uint8_t snapshot[sizeof(user_data)];
	k_mutex_lock(&save_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&data_lock);
	int err = data_error;
	bool changed = dirty;
	if (!err && changed) {
		memcpy(snapshot, user_data, sizeof(snapshot));
		/* Further writes mark a new change. A failed commit restores this one;
		 * save_lock keeps another saver from observing an unfinished commit. */
		dirty = false;
	}
	k_spin_unlock(&data_lock, key);
	if (!err && changed) {
		struct fs_file_t file;
		fs_file_t_init(&file);
		err = fs_open(&file, DATA_STAGE, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
		if (!err) {
			ssize_t size = fs_write(&file, snapshot, sizeof(snapshot));
			err = size < 0 ? size : size != sizeof(snapshot) ? -ENOSPC : 0;
			int closed = fs_close(&file);
			err = err ? err : closed;
		}
		if (!err) err = fs_rename(DATA_STAGE, DATA_PATH);
		if (err) {
			key = k_spin_lock(&data_lock);
			dirty = true;
			k_spin_unlock(&data_lock, key);
		}
	}
	k_mutex_unlock(&save_lock);
	return err;
}
