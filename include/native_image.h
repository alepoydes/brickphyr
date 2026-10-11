// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <zephyr/llext/loader.h>

#define NATIVE_IMAGE_LIMIT (32 * 1024)

/* Validate a bounded source without loading or executing it. The loader must
 * already be prepared. The returned size is the loader allocation budget. */
int native_image_check(struct llext_loader *loader, size_t length, size_t *heap_size);
