// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Initialize after mounting the common application store, preserving its error. */
void app_data_load(int storage_error);
/* Shared user data in RAM; no filesystem access or waiting in read/write. */
int app_data_read(uint32_t offset, void *data, size_t size);
int app_data_write(uint32_t offset, const void *data, size_t size);
/* Firmware threads only: atomically save a snapshot through the common store. */
int app_data_save(void);
