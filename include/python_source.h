// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>

/* Exact, bounded reads from an immutable package. The owner keeps the backing
 * RAM or file alive; readers retain offsets, never pointers into that backing. */
struct python_source {
	int (*read)(void *context, size_t offset, void *data, size_t length);
	void *context;
	size_t length;
};

struct python_resource {
	size_t offset, length;
};

enum python_resource_type { PYTHON_RESOURCE_MISSING, PYTHON_RESOURCE_FILE, PYTHON_RESOURCE_DIR };

int python_source_read(const struct python_source *source, size_t offset, void *data, size_t length);
int python_source_memory(void *context, size_t offset, void *data, size_t length);
int python_source_check(const struct python_source *source);
/* Return a resource type or the original I/O error. The empty path is the root;
 * the empty CBOR key is entry metadata, not a readable file. */
int python_source_find(const struct python_source *source, const char *path, struct python_resource *file);
int python_source_entry(const struct python_source *source, char *path, size_t capacity);
