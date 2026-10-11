// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <zephyr/fs/fs.h>

/* Format only an erased partition; preserve all unrecognized existing contents. */
int storage_mount_preserved(struct fs_mount_t *mount);
