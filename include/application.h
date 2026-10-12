// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum application_state { APPLICATION_IDLE, APPLICATION_RUNNING, APPLICATION_RETURNED, APPLICATION_STOPPED, APPLICATION_FAULT };
enum application_format { APPLICATION_NATIVE, APPLICATION_PYTHON, APPLICATION_MPY, APPLICATION_FILES,
	/* Internal transfer format, normalized to FILES before admission. */
	APPLICATION_PYBRICKS };

#define APPLICATION_IMAGE_LIMIT (32 * 1024)
#define APPLICATION_MEMORY_LIMIT (64 * 1024)

struct application_status {
	enum application_state state;
	enum application_format format;
	int result;
	unsigned int fault;
	int cleanup_error;
	size_t image_bytes;
	size_t loader_bytes;
	size_t loaded_bytes;
	size_t code_bytes;
	size_t code_used;
	size_t stack_bytes;
	size_t stack_unused;
	uint32_t output_dropped;
	size_t output_bytes;
	size_t upload_bytes;
	size_t upload_size;
	enum application_format upload_format;
	bool upload_ready;
	bool repl;
};

/* Privileged lifecycle. All callers share this one instance. */
#ifdef CONFIG_BRICKPHYR_NATIVE
int application_run_native(const void *image, size_t length);
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
int application_check_file(const char *path, size_t length);
int application_run_file(const char *path);
/* Consume a validated persistent-format upload into a closed staging file.
 * Flash I/O does not hold the lifecycle lock; failure still consumes the upload. */
int application_write_upload(const char *path);
#endif

/* A complete upload remains available until run/discard/replacement or reboot.
 * Only incomplete uploads are discarded when their management session ends. */
int application_upload(enum application_format format, size_t offset, size_t total,
	const void *data, size_t length, size_t *next);
/* Retention permits explicit repeat without another transfer. */
int application_run_uploaded(bool retain);
#ifdef CONFIG_BRICKPHYR_PYTHON
/* Pybricks announces the size after its last fragment. */
int application_upload_finish(size_t total);
#endif
void application_discard_upload(void);
/* Nonblocking; call after accepted operations have finished at session close. */
void application_session_closed(void);
int application_stop(void);
int application_shutdown(void);
void application_poll(void);
void application_get_status(struct application_status *status);
/* No cleanup, expiration, output consumption or wait; EBUSY during lifecycle work. */
int application_observe(struct application_status *status);
size_t application_output(void *data, size_t capacity);
#ifdef CONFIG_BRICKPHYR_PYTHON
/* Input is accepted atomically; EAGAIN leaves it wholly with the sender.
 * Interrupt discards queued input and asks the VM to raise KeyboardInterrupt. */
int application_input(const void *data, size_t length);
int application_interrupt(void);
/* Start a new REPL or reuse its live state; never replace another program. */
int application_repl(void);
#endif
#else
static inline int application_shutdown(void) { return 0; }
static inline void application_poll(void) { }
static inline void application_session_closed(void) { }
#endif

#ifdef CONFIG_BRICKPHYR_PYTHON
/* The selected continuous output consumer owns this attachment. A session
 * close releases backpressure without stopping the autonomous application. */
void application_output_stream(bool attached);
#else
static inline void application_output_stream(bool attached) { }
#endif
