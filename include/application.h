// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <brickphyr_host.h>

enum application_state { APPLICATION_IDLE, APPLICATION_RUNNING, APPLICATION_RETURNED, APPLICATION_STOPPED, APPLICATION_FAULT };
enum application_format { APPLICATION_NATIVE, APPLICATION_PYTHON, APPLICATION_MPY, APPLICATION_FILES,
	/* Internal transfer format, normalized to FILES before admission. */
	APPLICATION_PYBRICKS };

#define APPLICATION_IMAGE_LIMIT CONFIG_BRICKPHYR_APPLICATION_IMAGE_SIZE
#define NATIVE_MEMORY_LIMIT CONFIG_BRICKPHYR_NATIVE_MEMORY_SIZE

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
	size_t control_stack_bytes;
	int control_stack_unused;
	uint32_t output_dropped;
	size_t output_bytes;
	size_t upload_bytes;
	size_t upload_size;
	enum application_format upload_format;
	bool upload_ready;
	bool repl;
	int32_t program_id;
};

/* Current reservations, unlike status (which retains the previous run). These
 * components exclude allocator metadata; heap totals include all owners. */
struct application_memory {
	size_t heap_total, heap_used, heap_free, heap_peak;
	size_t source_ram, source_flash, staging, python_arena;
	size_t primary_native, control_native, code, code_used, records;
	int gc_used, gc_free, gc_age_ms;
	size_t stack_min, stack_max, stack_default;
	bool active;
};

struct application_stack_info {
	char name[32];
	enum { APPLICATION_STACK_KERNEL, APPLICATION_STACK_PRIMARY, APPLICATION_STACK_CONTROL } kind;
	int requested, user, unused, privileged, priv_unused;
	int next;
	bool present;
};

struct application_host_descriptor {
	uint64_t run;
	struct bp_host_type type;
	uint32_t last;
	bool present;
};

struct application_host_reply {
	uint32_t request;
	uint8_t state;
	int result;
	uint32_t detail;
};

/* Preflight accounting, not a fit guarantee. Native bytes are the admission
 * bound; stacks use the target's rounding. Allocator/object overhead and runtime
 * requests remain separate. No image linking or application execution. */
struct application_budget {
	enum application_format format;
	size_t image, source_ram, source_flash, transfer;
	size_t stack, user_stack, priv_stack, arena, code_limit;
	size_t native, alignment, supervisor;
};
struct application_budget_request {
	const char *resource;
	size_t stack, parameters;
	bool control;
};

/* Privileged lifecycle. All callers share this one instance. */
#ifdef CONFIG_BRICKPHYR_NATIVE
int application_run_native(const void *image, size_t length);
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
int application_check_file(const char *path, size_t length);
/* Reject mutation of a retained package; caller holds the store lock. */
int application_file_idle(const char *path);
int application_run_file(const char *path, int program_id, size_t stack_size);
/* Consume a validated persistent-format upload into a closed staging file.
 * Flash I/O does not hold the lifecycle lock; failure still consumes the upload. */
int application_write_upload(const char *path);
#endif

/* A complete upload remains available until run/discard/replacement or reboot.
 * Only incomplete uploads are discarded when their management session ends. */
int application_upload(enum application_format format, size_t offset, size_t total,
	const void *data, size_t length, size_t *next);
/* Retention permits explicit repeat without another transfer. */
int application_run_uploaded(bool retain, size_t stack_size);
#ifdef CONFIG_BRICKPHYR_PYTHON
/* Pybricks announces the size after its last fragment. */
int application_upload_finish(size_t total);
#endif
void application_discard_upload(void);
/* Nonblocking; call after accepted operations have finished at session close. */
void application_session_closed(void);
uint32_t application_session_id(void);
int application_stop(void);
/* Discard retained records only after the application has terminated. */
int application_clear_recording(void);
#ifdef CONFIG_BRICKPHYR_CAPTURE
struct bp_record_spec;
/* Reserve the idle recorder for a built-in capture. Existing records are never
 * discarded implicitly. The capture owner joins its worker before release. */
int application_capture_begin(const struct bp_record_spec *spec, const void *config, size_t size,
	uint32_t full_reason, size_t payload_bytes);
int application_capture_release(void);
#endif
/* Close application admission and quiesce resources without requesting power-off.
 * Reboot/DFU callers must retain power and their connection until reset. */
int application_shutdown(void);
void application_poll(void);
/* Called by the hub UI with zero while the connection menu consumes buttons. */
void application_buttons(uint32_t buttons);
void application_get_status(struct application_status *status);
/* No cleanup, expiration, output consumption or wait; EBUSY during lifecycle work. */
int application_observe(struct application_status *status);
/* Read-only snapshots; EBUSY during lifecycle work. No background sampler. */
int application_memory_get(struct application_memory *memory);
int application_stack_get(size_t index, struct application_stack_info *stack);
int application_host_descriptor_get(size_t index, struct application_host_descriptor *value);
int application_host_send(uint64_t run, uint16_t type, uint32_t request,
	const void *data, size_t length, struct application_host_reply *reply);
int application_host_status(uint64_t run, uint16_t type, uint32_t request,
	struct application_host_reply *reply);
int application_budget_uploaded(const struct application_budget_request *request,
	struct application_budget *budget);
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
/* Caller holds store_lock. */
int application_budget_file(const char *path, const struct application_budget_request *request,
	struct application_budget *budget);
#endif
size_t application_output(void *data, size_t capacity);
/* Drain output and capture its remaining count/state against one lifecycle.
 * Like get_status, this may finish cleanup of a terminated application. */
size_t application_read_output(void *data, size_t capacity, struct application_status *status);
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
static inline void application_buttons(uint32_t buttons) { }
static inline void application_session_closed(void) { }
static inline uint32_t application_session_id(void) { return 0; }
#endif

#ifdef CONFIG_BRICKPHYR_PYTHON
/* The selected continuous output consumer owns this attachment. A session
 * close releases backpressure without stopping the autonomous application. */
void application_output_stream(bool attached);
/* Only the selected Pybricks consumer attaches this mailbox. Zero detaches;
 * otherwise capacity is the transport's current event-data limit. */
void application_message_attach(size_t capacity);
int application_message_receive(size_t offset, const void *data, size_t length);
/* A copied event stays pending until transport acceptance. A replacement,
 * close or disconnect invalidates its token before a queued retry. */
size_t application_message_event(void *data, size_t capacity, uint32_t *request);
bool application_message_pending(uint32_t request);
void application_message_sent(uint32_t request);
#else
static inline void application_output_stream(bool attached) { }
static inline void application_message_attach(size_t capacity) { }
#endif
