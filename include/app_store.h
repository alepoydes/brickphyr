// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>

struct k_mutex;
extern struct k_mutex store_lock;
/* Caller owns store_lock. Resolve the common installed namespace. */
int app_store_path(const char *name, char *path, size_t capacity);

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
int app_store_run(const char *name, size_t stack_size);
struct application_budget_request;
struct application_budget;
int app_store_budget(const char *name, const struct application_budget_request *request,
	struct application_budget *budget);
