// SPDX-License-Identifier: Apache-2.0
#include "app_data.h"
#include "storage_work.h"
#include <brickphyr_system.h>
#include <limits.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_data);
static K_MUTEX_DEFINE(save_lock);
static struct k_spinlock data_lock;
static uint8_t user_data[BP_USER_DATA_SIZE];
static uint8_t snapshot[BP_USER_DATA_SIZE];
static bool dirty;
static int data_error = -EAGAIN;
static uint32_t save_request;
static int save_result;
static bool firmware_saving;
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

static void save_worker(struct k_work *work)
{
	struct fs_file_t file;
	fs_file_t_init(&file);
	int err = fs_open(&file, DATA_STAGE, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (!err) {
		ssize_t size = fs_write(&file, snapshot, sizeof(snapshot));
		err = size < 0 ? size : size != sizeof(snapshot) ? -ENOSPC : 0;
		int closed = fs_close(&file);
		err = err ? err : closed;
	}
	if (!err) err = fs_rename(DATA_STAGE, DATA_PATH);
	k_spinlock_key_t key = k_spin_lock(&data_lock);
	if (err) dirty = true;
	save_result = err;
	k_spin_unlock(&data_lock, key);
}
static K_WORK_DEFINE(save_work, save_worker);

static int save_start(bool firmware)
{
	k_spinlock_key_t key = k_spin_lock(&data_lock);
	int err = data_error ? data_error :
		(save_result == -EINPROGRESS || (firmware_saving && !firmware)) ? -EBUSY :
		save_request == INT_MAX ? -EOVERFLOW : 0;
	if (!err && dirty) {
		memcpy(snapshot, user_data, sizeof(snapshot));
		/* Submit under the same lock: an aborted caller cannot leave a pending
		 * snapshot without a worker. No application pointers survive this call. */
		err = storage_work_submit(&save_work);
		if (err >= 0) {
			dirty = false;
			save_result = -EINPROGRESS;
		}
	} else if (!err) {
		save_result = 0;
	}
	int request = err < 0 ? err : ++save_request;
	k_spin_unlock(&data_lock, key);
	return request;
}

int app_data_save_start(void) { return save_start(false); }

int app_data_save_status(uint32_t request)
{
	k_spinlock_key_t key = k_spin_lock(&data_lock);
	int result = !request || request > INT_MAX ? -EINVAL :
		request != save_request ? -ESTALE : save_result;
	k_spin_unlock(&data_lock, key);
	return result;
}

int app_data_save(void)
{
	k_mutex_lock(&save_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&data_lock);
	/* Shell/shutdown owns this result until read, even if an application keeps
	 * requesting asynchronous saves after the worker finishes. */
	firmware_saving = true;
	k_spin_unlock(&data_lock, key);
	struct k_work_sync sync;
	/* Shutdown first stops writers. Join their accepted save before committing
	 * subsequent RAM changes; never cut power beneath the filesystem worker. */
	(void)k_work_flush(&save_work, &sync);
	int request = save_start(true);
	int err = request < 0 ? request : 0;
	if (!err) {
		(void)k_work_flush(&save_work, &sync);
		err = app_data_save_status(request);
	}
	key = k_spin_lock(&data_lock);
	firmware_saving = false;
	k_spin_unlock(&data_lock, key);
	k_mutex_unlock(&save_lock);
	return err;
}
