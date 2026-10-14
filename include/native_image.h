// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "application.h"

/* Read exactly size bytes at offset, or return a negative error. Admission
 * bounds every read before invoking this callback; the source must stay fixed.
 * It needs no linker context or allocation. heap_size is written on success. */
typedef int (*native_image_read_fn)(void *context, size_t offset, void *data, size_t size);
int native_image_check(native_image_read_fn read, void *context, size_t length, size_t *heap_size);
int native_image_check_buffer(const void *data, size_t length, size_t *heap_size);
