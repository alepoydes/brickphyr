// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Initialize after mounting the common application store, preserving its error. */
void app_data_load(int storage_error);
/* Shared user data in RAM; no filesystem access or waiting in read/write. */
int app_data_read(uint32_t offset, void *data, size_t size);
int app_data_write(uint32_t offset, const void *data, size_t size);
/* Copy a snapshot and submit it without waiting. Positive request or errno;
 * only one pending request. It survives application exit and disconnect. */
int app_data_save_start(void);
/* Latest request: EINPROGRESS, success or stored error; ESTALE after a newer
 * accepted request. No waiting or filesystem access. */
int app_data_save_status(uint32_t request);
/* Firmware threads only: wait for prior work, then save the current snapshot. */
int app_data_save(void);
