// SPDX-License-Identifier: Apache-2.0
#include "storage_mount.h"
#include <zephyr/storage/flash_map.h>

int storage_mount_preserved(struct fs_mount_t *mount)
{
	const struct flash_area *area;
	int err = flash_area_open((uintptr_t)mount->storage_dev, &area);
	if (err) {
		return err;
	}
	/* Only an entirely erased partition may be formatted automatically. A failed
	 * mount of existing data must never erase another firmware's user storage. */
	uint8_t bytes[256];
	bool empty = true;
	for (size_t offset = 0; empty && offset < area->fa_size; offset += sizeof(bytes)) {
		size_t length = MIN(sizeof(bytes), area->fa_size - offset);
		err = flash_area_read(area, offset, bytes, length);
		if (err) {
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
	if (!err && empty) {
		err = fs_mkfs(FS_LITTLEFS, (uintptr_t)mount->storage_dev, mount->fs_data, 0);
	}
	if (!err) {
		err = fs_mount(mount);
	}
	return err;
}
