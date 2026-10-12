// SPDX-License-Identifier: Apache-2.0
#include "app_store.h"
#include "application.h"
#include "storage_mount.h"

#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#ifdef CONFIG_MCUMGR_GRP_FS
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt.h>
#endif

LOG_MODULE_REGISTER(app_store);
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(filesystem);
static struct fs_mount_t mount = {
	.type = FS_LITTLEFS,
	.fs_data = &filesystem,
	.storage_dev = (void *)PARTITION_ID(apps_partition),
	.mnt_point = "/apps",
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};
K_MUTEX_DEFINE(store_lock);
static int status = -EAGAIN;

static bool name_valid(const char *name)
{
	size_t length = strlen(name);
	if (!length || length > APP_NAME_MAX || name[0] == '.') {
		return false;
	}
	/* Keep slots readable/removable in builds without Python. Their reserved
	 * namespace could not be used by earlier named applications. */
	if (name[0] == '@') {
		if (!name[1] || (name[1] == '0' && name[2])) {
			return false;
		}
		unsigned int slot = 0;
		for (size_t i = 1; i < length; i++) {
			if (name[i] < '0' || name[i] > '9') {
				return false;
			}
			slot = slot * 10 + name[i] - '0';
			if (slot > 127) {
				return false;
			}
		}
		return true;
	}
	for (size_t i = 0; i < length; i++) {
		char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
			return false;
		}
	}
	return true;
}

static int path_for(const char *name, char *path, size_t capacity)
{
	if (!name_valid(name)) {
		return -EINVAL;
	}
	snprintk(path, capacity, "/apps/%s", name);
	return status;
}

int app_store_status(void)
{
	k_mutex_lock(&store_lock, K_FOREVER);
	int err = status;
	k_mutex_unlock(&store_lock);
	return err;
}

static int initialize(void)
{
	status = storage_mount_preserved(&mount);
	if (status) {
		LOG_ERR("Application storage unavailable: %d; existing data preserved", status);
	}
	return status;
}
SYS_INIT(initialize, APPLICATION, 81);

int app_store_format(void)
{
	k_mutex_lock(&store_lock, K_FOREVER);
	/* Never erase a mounted store; remove individual applications instead. */
	int err = status ? fs_mkfs(FS_LITTLEFS, (uintptr_t)mount.storage_dev, &filesystem, 0) :
		-EBUSY;
	if (!err) {
		status = fs_mount(&mount);
		err = status;
	}
	k_mutex_unlock(&store_lock);
	return err;
}

int app_store_next(const char *after, struct app_file *entry, size_t *available)
{
	if (*after && !name_valid(after)) {
		return -EINVAL;
	}
	k_mutex_lock(&store_lock, K_FOREVER);
	int err = status;
	struct fs_dir_t directory;
	fs_dir_t_init(&directory);
	*entry = (struct app_file){0};
	if (!err) {
		err = fs_opendir(&directory, mount.mnt_point);
		if (!err) {
			struct fs_dirent item;
			while (!(err = fs_readdir(&directory, &item)) && item.name[0]) {
				if (item.type == FS_DIR_ENTRY_FILE && name_valid(item.name) &&
				    strcmp(item.name, after) > 0 &&
				    (!entry->name[0] || strcmp(item.name, entry->name) < 0)) {
					strcpy(entry->name, item.name);
					entry->size = item.size;
				}
			}
			int closed = fs_closedir(&directory);
			err = err ? err : closed;
		}
	}
	if (!err) {
		struct fs_statvfs space;
		err = fs_statvfs(mount.mnt_point, &space);
		if (!err) {
			*available = space.f_bfree * space.f_frsize;
		}
	}
	k_mutex_unlock(&store_lock);
	return err;
}

int app_store_remove(const char *name)
{
	char path[sizeof("/apps/") + APP_NAME_MAX];
	k_mutex_lock(&store_lock, K_FOREVER);
	int err = path_for(name, path, sizeof(path));
	if (!err) {
		err = fs_unlink(path);
	}
	k_mutex_unlock(&store_lock);
	return err;
}

int app_store_publish(const char *name, size_t length)
{
	if (name[0] == '@') {
		return -EACCES;
	}
	char path[sizeof("/apps/") + APP_NAME_MAX];
	k_mutex_lock(&store_lock, K_FOREVER);
	int err = path_for(name, path, sizeof(path));
	struct fs_dirent entry;
	if (!err) {
		err = fs_stat(APP_STAGE, &entry);
	}
	if (!err && (entry.size != length || entry.type != FS_DIR_ENTRY_FILE)) {
		err = -EINVAL;
	}
	if (!err) {
		err = application_check_file(APP_STAGE, length);
	}
	if (!err) {
		/* LittleFS commits this replacement atomically; old content remains
		 * visible until the fully validated incoming image takes its name. */
		err = fs_rename(APP_STAGE, path);
	}
	k_mutex_unlock(&store_lock);
	return err;
}

int app_store_publish_upload(const char *name)
{
	char path[sizeof("/apps/") + APP_NAME_MAX];
	k_mutex_lock(&store_lock, K_FOREVER);
	int err = path_for(name, path, sizeof(path));
#ifdef CONFIG_MCUMGR_GRP_FS
	if (!err) {
		/* Profile handover can leave a partial standard upload. Close its
		 * public handle before reusing the common staging path. */
		err = fs_mgmt_file_close();
	}
#endif
	if (!err) {
		err = application_write_upload(APP_STAGE);
	}
	if (!err) {
		err = fs_rename(APP_STAGE, path);
	}
	k_mutex_unlock(&store_lock);
	return err;
}

int app_store_run(const char *name)
{
	char path[sizeof("/apps/") + APP_NAME_MAX];
	k_mutex_lock(&store_lock, K_FOREVER);
	int err = path_for(name, path, sizeof(path));
	if (!err) {
		err = application_run_file(path);
	}
	k_mutex_unlock(&store_lock);
	return err;
}

#ifdef CONFIG_MCUMGR
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt_callbacks.h>

static enum mgmt_cb_return file_access(uint32_t event, enum mgmt_cb_return previous,
		int32_t *rc, uint16_t *group, bool *abort, void *data, size_t size)
{
	ARG_UNUSED(event); ARG_UNUSED(group); ARG_UNUSED(abort); ARG_UNUSED(size);
	if (previous != MGMT_CB_OK) {
		return previous;
	}
	struct fs_mgmt_file_access *access = data;
	const char *path = access->filename;
	bool stage = !strcmp(path, APP_STAGE);
	bool allowed = !app_store_status() && (stage ||
		(access->access != FS_MGMT_FILE_ACCESS_WRITE &&
		 !strncmp(path, "/apps/", 6) && name_valid(path + 6)));
	if (!allowed) {
		*rc = MGMT_ERR_EACCESSDENIED;
		return MGMT_CB_ERROR_RC;
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback access_callback = {
	.callback = file_access,
	.event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS,
};
static int register_access(void)
{
	mgmt_callback_register(&access_callback);
	return 0;
}
SYS_INIT(register_access, APPLICATION, 82);
#endif
