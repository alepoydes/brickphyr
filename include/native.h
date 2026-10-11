// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum native_state { NATIVE_IDLE, NATIVE_RUNNING, NATIVE_RETURNED, NATIVE_STOPPED, NATIVE_FAULT };

struct native_status {
	enum native_state state;
	int result;
	unsigned int fault;
	int cleanup_error;
	size_t image_bytes;
	size_t loader_bytes;
	size_t loaded_bytes;
	size_t stack_bytes;
	size_t stack_unused;
	uint32_t output_dropped;
	size_t output_bytes;
	size_t upload_bytes;
	size_t upload_size;
	bool upload_ready;
};

/* Privileged lifecycle. All callers share this one instance. */
#ifdef CONFIG_BRICKPHYR_NATIVE
int native_run(const void *image, size_t length);
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
int native_run_file(const char *path);
#endif
/* A complete upload remains available until run/discard/replacement or reboot.
 * Only incomplete uploads are discarded when their management session ends. */
int native_upload(size_t offset, size_t total, const void *data, size_t length, size_t *next);
int native_run_uploaded(void);
void native_discard_upload(void);
/* Nonblocking; call after accepted operations have finished at session close. */
void native_session_closed(void);
int native_stop(void);
int native_shutdown(void);
void native_poll(void);
void native_get_status(struct native_status *status);
size_t native_output(void *data, size_t capacity);
#else
static inline int native_shutdown(void) { return 0; }
static inline void native_poll(void) { }
static inline void native_session_closed(void) { }
#endif
