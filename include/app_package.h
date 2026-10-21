// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include "application.h"

#define APP_PATH_LIMIT 128
#define APP_FILE_LIMIT 64

/* Exact, bounded reads from an immutable package. The owner keeps the backing
 * RAM or file alive; readers retain offsets, never pointers into that backing. */
struct app_source {
	int (*read)(void *context, size_t offset, void *data, size_t length);
	void *context;
	size_t length;
};

struct app_resource {
	size_t offset, length;
};

enum app_resource_type { APP_RESOURCE_MISSING, APP_RESOURCE_FILE, APP_RESOURCE_DIR };

int app_source_read(const struct app_source *source, size_t offset, void *data, size_t length);
int app_source_memory(void *context, size_t offset, void *data, size_t length);
struct app_package_info {
	enum application_format format;
	char entry[APP_PATH_LIMIT + 1];
};

/* FILES denotes ordinary data; executable envelopes use NATIVE/PYTHON/MPY.
 * Validation reads bounded ranges and does not link or execute components. */
typedef int (*app_package_check_fn)(const struct app_source *source,
	struct app_resource file, enum application_format format);
int app_package_check(const struct app_source *source, app_package_check_fn check,
	struct app_package_info *info);
/* Return a resource type or the original I/O error. The empty path is the root;
 * the empty CBOR key is entry metadata, not a readable file. */
int app_package_find(const struct app_source *source, const char *path, struct app_resource *file);
int app_package_entry(const struct app_source *source, char *path, size_t capacity);
