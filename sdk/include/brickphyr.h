// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>
#include <zephyr/syscall.h>
#include <brickphyr_abi.h>

/* Every image carries the identity generated with its SDK. */
#define BRICKPHYR_APPLICATION() \
	static const char __attribute__((section(".brickphyr.abi"), used)) \
		brickphyr_application_abi[] = BRICKPHYR_ABI

/* Monotonic milliseconds since hub boot. No allocation. */
__syscall int64_t bp_time_ms(void);
/* Sleep in milliseconds (0 yields); at most INT32_MAX. Stop can cancel it. */
__syscall int bp_sleep(uint32_t milliseconds);
/* Append at most 128 bytes without blocking. Returns accepted length or errno.
 * A full queue drops the unaccepted suffix and counts it in application status. */
__syscall int bp_write(const void *data, size_t length);
/* Bit 0 center, 1 left, 2 right, 3 Bluetooth. Read-only, no allocation. */
__syscall uint32_t bp_buttons(void);

/* The firmware calls this entry in user mode; return ends the application. */
int app_main(void);

#include <zephyr/syscalls/brickphyr.h>
