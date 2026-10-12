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
/* One pending code reservation; commit(NULL, 0) cancels it after a loader error. */
__syscall intptr_t application_code_reserve(size_t length);
__syscall int application_code_commit(const void *data, size_t length);
#endif
#include <zephyr/syscalls/application_private.h>
