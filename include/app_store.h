// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>

#define APP_NAME_MAX 32
#define APP_STAGE "/apps/.incoming"
struct app_file {
	char name[APP_NAME_MAX + 1];
	size_t size;
};

int app_store_status(void);
int app_store_format(void);
/* Lexical pagination; an empty returned name ends iteration. */
int app_store_next(const char *after, struct app_file *entry, size_t *available);
int app_store_remove(const char *name);
/* Called by the serial SMP dispatcher, after closing its standard FS transfer. */
int app_store_publish(const char *name, size_t length);
/* Install the common validated RAM upload; also owns reserved @0..@127 slots. */
int app_store_publish_upload(const char *name);
int app_store_run(const char *name);
