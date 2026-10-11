// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>
#include <zephyr/syscall.h>
#include <brickphyr_abi.h>
#include <brickphyr_matrix.h>
#include <brickphyr_imu.h>

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

/* Queue 1..25 brightness values at row-major offset first. Copies before return;
 * no allocation or wait. 0 means accepted, not yet applied; EBUSY means a frame
 * is pending or the terminal owns the matrix. Ownership lasts until app exit.
 * Unsupported builds return ENOTSUP. Errors are negative errno throughout.
 */
__syscall int bp_matrix_write(uint32_t first, const uint8_t *values, size_t count);
/* Bounded snapshot including the last completed write, timestamp and I/O error. */
__syscall int bp_matrix_get(struct bp_matrix_state *state);

/* Latest uncalibrated IMU sample. Copies even when sample.error is nonzero;
 * does not acquire data or change sensor configuration. ENOTSUP if excluded. */
__syscall int bp_imu_get(struct bp_imu_sample *sample);

/* The firmware calls this entry in user mode; return ends the application. */
int app_main(void);

#include <zephyr/syscalls/brickphyr.h>
