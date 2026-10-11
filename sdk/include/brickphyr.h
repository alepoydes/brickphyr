// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>
#include <zephyr/syscall.h>
#include <brickphyr_abi.h>
#include <brickphyr_matrix.h>
#include <brickphyr_imu.h>
#include <brickphyr_sound.h>
#include <brickphyr_battery.h>
#include <brickphyr_port.h>

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
/* Calibrated vectors and orientation paired with their original raw sample.
 * Same bounded read and error convention. Orientation continuity uses epoch. */
__syscall int bp_imu_get_state(struct bp_imu_state *state);

/* Copy the latest battery pair without starting ADC or waiting for acquisition.
 * Check sample.error; startup, stale, failed and stopped readings stay explicit. */
__syscall int bp_battery_get(struct bp_battery_sample *sample);

/* Copy electrical detection state; no I/O or port reconfiguration. Ports are
 * numbered 0..5 (A..F). This does not negotiate a UART device or drive a motor. */
__syscall int bp_port_get(uint32_t port, struct bp_port_state *state);

/* Acquire the free speaker and queue a square tone: 20..20000 Hz, 0..100 volume.
 * Zero duration continues until stopped; otherwise duration is milliseconds.
 * All sound commands are nonblocking; completion/error is visible in get(). */
__syscall int bp_sound_tone(uint32_t frequency, uint32_t duration_ms, uint32_t volume);
/* Acquire the free speaker for signed 16-bit mono PCM, 8000..48000 samples/s.
 * Playback starts after two chunks are queued, or finish marks a shorter stream. */
__syscall int bp_sound_stream(uint32_t rate);
/* Copy 1..BP_SOUND_CHUNK_SAMPLES. 0 accepts all, EAGAIN accepts none.
 * Buffers can be reused immediately. An unfilled running stream stops with EPIPE. */
__syscall int bp_sound_write(const int16_t *samples, size_t count);
/* End input and drain queued samples. Application exit still stops playback;
 * keep the application alive until get() reports idle to hear the complete stream. */
__syscall int bp_sound_finish(void);
__syscall int bp_sound_stop(void);
__syscall int bp_sound_get(struct bp_sound_state *state);

/* The firmware calls this entry in user mode; return ends the application. */
int app_main(void);

#include <zephyr/syscalls/brickphyr.h>
