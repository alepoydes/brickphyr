// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>
#include <zephyr/syscall.h>
#include <brickphyr_abi.h>
#include <brickphyr_matrix.h>
#include <brickphyr_light.h>
#include <brickphyr_imu.h>
#include <brickphyr_sound.h>
#include <brickphyr_battery.h>
#include <brickphyr_port.h>
#include <brickphyr_sensor.h>
#include <brickphyr_motor.h>
#include <brickphyr_system.h>

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
/* Read the calling application's launch identity and current management link.
 * No I/O or wait; name and boot cause share the firmware's existing providers. */
__syscall int bp_system_get(struct bp_system_info *info);
/* Select a stop-button combination using bp_buttons() bits; 0 disables it.
 * New presses are required. Python receives SystemExit; native apps are stopped.
 * This never disables long-center-press power-off or host-forced stop. */
__syscall int bp_set_stop_buttons(uint32_t buttons);
/* Read the current combination, for temporary changes followed by restoration. */
__syscall uint32_t bp_get_stop_buttons(void);
/* Request the ordinary shutdown/cleanup path; this call does not return. */
__syscall void bp_shutdown(void);
/* Shared user bytes, copied in RAM. Persist at orderly shutdown or with the
 * firmware's explicit storage-save operation; no flash I/O in this caller. */
__syscall int bp_user_data_read(uint32_t offset, void *data, size_t size);
__syscall int bp_user_data_write(uint32_t offset, const void *data, size_t size);

/* Queue logical RGB (0..255 per channel), copied before return. Claims one
 * button light until application exit. No allocation, I/O or wait; a pending
 * request returns EBUSY. Connection menus and shutdown override the app color.
 * Cleanup restores system indication. get() reports completion and visibility.
 */
__syscall int bp_light_write(uint32_t light, const uint8_t *rgb);
__syscall int bp_light_get(uint32_t light, struct bp_light_state *state);
/* Repeat 1..BP_LIGHT_MAX_FRAMES, copied into firmware-owned memory. Allocates
 * without waiting; validation/allocation failure preserves the previous light.
 * Static writes cancel animation. Menus override visibility, not its clock;
 * delayed updates skip expired frames. An I/O failure stops advancement.
 */
__syscall int bp_light_animate(uint32_t light, const struct bp_light_frame *frames, size_t count);

/* Queue 1..25 brightness values at row-major offset first. Copies before return;
 * no allocation or wait. 0 means accepted, not yet applied; EBUSY means a frame
 * is pending or the terminal owns the matrix. Ownership lasts until app exit.
 * Static writes stop animation; a partial write then starts on a blank matrix.
 * Coordinates use the application's selected orientation (upright by default).
 * Unsupported builds return ENOTSUP. Errors are negative errno throughout.
 */
__syscall int bp_matrix_write(uint32_t first, const uint8_t *values, size_t count);
/* Bounded snapshot including the last completed write, timestamp and I/O error. */
__syscall int bp_matrix_get(struct bp_matrix_state *state);
/* Repeat 1..255 row-major frames of 25 brightness bytes at 1..INT32_MAX ms.
 * Copies into firmware-owned memory without waiting. Invalid input, busy or
 * allocation failure preserves the previous display. Delayed updates skip
 * expired frames; an I/O error stops animation. Cleanup frees the copy.
 */
__syscall int bp_matrix_animate(const uint8_t *frames, size_t count, uint32_t interval_ms);
/* Select 0..3 clockwise quarter-turns for subsequent application writes and
 * animation frames. Does not redraw, allocate or acquire the display. Reset to
 * zero on application cleanup; terminal coordinates remain physical.
 */
__syscall int bp_matrix_orientation(uint32_t rotation);

/* Latest uncalibrated IMU sample. Copies even when sample.error is nonzero;
 * does not acquire data or change sensor configuration. ENOTSUP if excluded. */
__syscall int bp_imu_get(struct bp_imu_sample *sample);
/* Calibrated vectors and orientation paired with their original raw sample.
 * Same bounded read and error convention. Orientation continuity uses epoch. */
__syscall int bp_imu_get_state(struct bp_imu_state *state);
/* Copy a frame or calibration command for asynchronous execution. EBUSY preserves
 * the outstanding command. Invalid input has no effects. No flash writes.
 * Observe pending/command_error with get_application. Cleanup restores axes
 * and heading independently of finalizers; selected calibration remains in RAM.
 * Frame changes do not alter physical observations or their orientation epoch. */
__syscall int bp_imu_configure(const struct bp_imu_command *command);
/* Coherent physical sample and application frame. Check physical.error/epoch
 * before comparing angles; raw angles cannot bridge missing measurements. */
__syscall int bp_imu_get_application(struct bp_imu_application_state *state);

/* Copy cached battery observations without I/O or waiting for acquisition.
 * Check error for voltage/current, temperature_error for temperature. Chemistry
 * is independent of both; startup, stale, failed and stopped readings are explicit. */
__syscall int bp_battery_get(struct bp_battery_sample *sample);

/* Copy port identity and raw UART data; no I/O or port reconfiguration.
 * Ports are numbered 0..5 (A..F). */
__syscall int bp_port_get(uint32_t port, struct bp_port_state *state);
/* Read one advertised mode after negotiation. Use generation from bp_port_get;
 * ESTALE means a different connection. No mode change, ownership or device I/O. */
__syscall int bp_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *info);
/* Claim an idle UART port and select an advertised mode without writing data
 * or driving a motor. Generation guards hotplug; 0 means queued. Wait for
 * bp_port_get().mode_error == 0 and the same generation/selected_mode/mode.
 * Other claims, active motor control and invalid modes fail before changes. */
__syscall int bp_port_select(uint32_t port, uint32_t generation, uint32_t mode);
/* Shared converted measurement. Inspect error, fields, sample_ms and generation.
 * Reads never change modes or interfere with a running application. */
__syscall int bp_sensor_get(uint32_t port, struct bp_sensor_sample *sample);
/* Claim this sensor's measurement mode until release/app exit. Nonblocking:
 * 0 means accepted; get() reports EAGAIN until a settled sample is available.
 * Unsupported measurements return ENOTSUP before a command or claim. */
__syscall int bp_sensor_configure(uint32_t port, uint32_t measurement);
/* Release the port claim: coast its motor and restore automatic measurement.
 * Accepted asynchronously; motor state retains ownership until coast succeeds.
 * bp_sensor_release is the existing sensor-side spelling of this operation. */
__syscall int bp_port_release(uint32_t port);
__syscall int bp_sensor_release(uint32_t port);

/* Queue direct drive, -1000..1000 permille in the device's direction. Zero
 * brakes. Optional duration ends in coast; 0 continues until stopped/released.
 * At most INT32_MAX ms. One pending command per port; EBUSY accepts nothing.
 * There is one shared port owner and no simultaneous multi-port commit. */
__syscall int bp_motor_drive(uint32_t port, int32_t duty, uint32_t duration_ms);
/* May supersede a pending drive; retains the application's port claim. */
__syscall int bp_motor_coast(uint32_t port);
/* Latest command/output result, no I/O. Inspect pending, valid and error. */
__syscall int bp_motor_get(uint32_t port, struct bp_motor_state *state);
/* Queue a regulated speed/position command; does not wait for motion. The
 * command is copied. bp_motor_get reports driver completion separately from
 * trajectory completion. Coast or release cancels regulation. */
__syscall int bp_motor_control(uint32_t port, const struct bp_motor_command *command);

/* Acquire the free speaker and queue a square tone: 20..24000 Hz, 0..100 volume.
 * Frequency zero produces silence. An idle reservation can start another tone.
 * Zero duration continues until stopped; otherwise duration is milliseconds.
 * All sound commands are nonblocking; completion/error is visible in get(). */
__syscall int bp_sound_tone(uint32_t frequency, uint32_t duration_ms, uint32_t volume);
/* Reserve the free speaker across tones/streams and pauses. Completion leaves
 * it idle but owned; stop or application cleanup releases it. No wait/allocation.
 * EBUSY leaves any existing operation unchanged, including this app's playback. */
__syscall int bp_sound_reserve(void);
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
