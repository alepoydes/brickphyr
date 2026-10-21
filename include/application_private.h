// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <zephyr/toolchain.h>
#include <zephyr/syscall.h>
__syscall void application_finished(int result);
#ifdef CONFIG_BRICKPHYR_PYTHON
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
/* Exact reads from this application's immutable source, serviced below control
 * priority. The kernel validates every offset and destination. */
__syscall int application_source_read(size_t offset, void *data, size_t length);

/* One pending code reservation; commit(NULL, 0) cancels it after a loader error. */
__syscall intptr_t application_code_reserve(size_t length);
__syscall int application_code_commit(const void *data, size_t length);
#endif
#include <zephyr/syscalls/application_private.h>
