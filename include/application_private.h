// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <zephyr/toolchain.h>
#include <zephyr/syscall.h>
__syscall void application_finished(int result);
#include <brickphyr_control.h>
#include "application_record.h"
#define APPLICATION_CONTROL_PARAMETERS_MAX 256
struct application_control_status {
	struct bp_control_step step;
	uint32_t revision, period_us, stack_bytes, loader_bytes, loaded_bytes;
};
#ifdef CONFIG_BRICKPHYR_PYTHON
__syscall int application_control_start(const void *image, size_t size,
	const void *parameters, size_t length);
__syscall int application_control_update(const void *parameters, size_t length);
__syscall int application_control_get(struct application_control_status *value);
struct application_record_request {
	uint64_t index;
	uint32_t generation;
	void *data, *parameters;
	size_t capacity, parameters_capacity;
};
__syscall int application_record_get(struct application_record_info *info);
__syscall int application_record_readinto(const struct application_record_request *request,
	struct application_record_info *info, struct bp_control_step *step);
/* Zero capacity observes pending bytes without consuming them. */
__syscall int application_stdin(void *data, size_t capacity);
/* Attached stream: return bytes accepted, zero when full; detached: consume into
 * the counted log. Zero length observes writable status without emitting bytes. */
__syscall int application_stdout(const void *data, size_t length);
/* Wake on input, output space, attachment, interrupt or timeout; -1 waits. */
__syscall int application_event_wait(int32_t timeout_ms);
/* One copied AppData mailbox, separate from standard input/output. Tokens bind
 * reads, finalizers and asynchronous completion to the originating object. */
#define APPLICATION_MESSAGE_RX_MAX 1024
#define APPLICATION_MESSAGE_TX_MAX 63
__syscall int application_message_open(size_t capacity);
__syscall int application_message_close(uint32_t lease);
__syscall int application_message_read(uint32_t lease, size_t offset, void *data, size_t length);
__syscall int application_message_send(uint32_t lease, const void *data, size_t length);
__syscall int application_message_status(uint32_t lease, uint32_t request);
__syscall int application_message_cancel(uint32_t lease, uint32_t request);
/* One pending code reservation; commit(NULL, 0) cancels it after a loader error. */
__syscall intptr_t application_code_reserve(size_t length);
__syscall int application_code_commit(const void *data, size_t length);
#endif
#include <zephyr/syscalls/application_private.h>
