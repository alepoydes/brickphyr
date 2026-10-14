// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <brickphyr_abi.h>
#include <brickphyr_matrix.h>
#include <brickphyr_light.h>
#include <brickphyr_radio.h>
#include <brickphyr_ble_peer.h>
#include <brickphyr_network.h>
#include <brickphyr_imu.h>
#include <brickphyr_sound.h>
#include <brickphyr_battery.h>
#include <brickphyr_charger.h>
#include <brickphyr_port.h>
#include <brickphyr_sensor.h>
#include <brickphyr_motor.h>
#include <brickphyr_drivebase.h>
#include <brickphyr_system.h>
#include <brickphyr_control.h>
#include <brickphyr_record.h>

#if defined(__ZEPHYR__) && !defined(LL_EXTENSION_BUILD)
#include <zephyr/toolchain.h>
#include <zephyr/syscall.h>

/* ABI v1 slots follow declaration order. Append new calls at the end; changing
 * existing signatures or public structure layouts requires a new ABI major. */

/* Monotonic milliseconds since hub boot. No allocation. */
__syscall int64_t bp_time_ms(void);
/* Sleep in milliseconds (0 yields); at most INT32_MAX. Stop can cancel it. */
__syscall int bp_sleep(uint32_t milliseconds);
/* Append at most 128 bytes without blocking. Returns accepted length or errno.
 * A full queue drops the unaccepted suffix and counts it in application status. */
__syscall int bp_write(const void *data, size_t length);
/* Bit 0 center, 1 left, 2 right, 3 Bluetooth. Read-only, no allocation. */
__syscall uint32_t bp_buttons(void);
/* One observer configuration per application, with up to 255 channel entries.
 * Allocate/copy without waiting; ready reports asynchronous scan startup.
 * Repeated identical packets refresh age/RSSI. Samples expire after 1000 ms.
 * Close retires the lease immediately and schedules scanner cleanup. */
__syscall int bp_radio_observe(const uint8_t *channels, uint32_t count, uint32_t *handle);
__syscall int bp_radio_ready(uint32_t handle);
/* Loaded controller firmware label, NUL-terminated; size includes the NUL. */
__syscall int bp_radio_version(char *value, uint32_t size);
__syscall int bp_radio_get(uint32_t handle, uint32_t channel, struct bp_radio_sample *sample);
__syscall int bp_radio_close(uint32_t handle);
/* One application BLE peer, separate from computer management. Open copies
 * its filter and allocates only the requested notification queue (0..255).
 * Connect/disconnect/write start cancellable asynchronous operations; get copies
 * busy/error and the typed snapshot. No application buffer reaches HCI.
 * read returns the oldest message size, or 0; overflow drops the oldest message.
 * close releases the lease, retaining an idle connected peer for reuse within
 * this application. Supervised application cleanup always disconnects it. */
__syscall int bp_ble_peer_open(const struct bp_ble_peer_config *config, uint32_t *handle);
__syscall int bp_ble_peer_connect(uint32_t handle);
__syscall int bp_ble_peer_disconnect(uint32_t handle);
__syscall int bp_ble_peer_get(uint32_t handle, struct bp_ble_peer_state *state);
__syscall int bp_lwp3_write(uint32_t handle, const void *data, uint32_t size);
__syscall int bp_lwp3_read(uint32_t handle, struct bp_lwp3_message *message);
__syscall int bp_ble_peer_close(uint32_t handle);
/* One HubNetwork inbox lease; memory is allocated per source. Close/stop
 * detaches queues and cancels pending work; established network links persist.
 * Connect/send return a positive request token, completed through get().
 * Cancel only affects the matching token. All messages are copied. */
__syscall int bp_network_open(uint32_t inbox_size, uint32_t *handle);
__syscall int bp_network_get(uint32_t handle, struct bp_network_state *state);
__syscall int bp_network_connect(uint32_t handle, const uint8_t *address);
__syscall int bp_network_send(uint32_t handle, const struct bp_network_message *message);
__syscall int bp_network_cancel(uint32_t handle, uint32_t request);
/* Snapshot returns an exclusive sequence bound; latest keeps one per source.
 * Read returns 1 with the oldest message before that bound, or 0 when empty. */
__syscall int bp_network_snapshot(uint32_t handle, uint32_t latest, uint32_t *through);
__syscall int bp_network_read(uint32_t handle, uint32_t through, struct bp_network_message *message);
__syscall int bp_network_close(uint32_t handle);
/* Xbox report-3 force feedback. Completes when the write is acknowledged,
 * without waiting for the physical effect. Unsupported inputs have no effects. */
__syscall int bp_xbox_rumble(uint32_t handle, const struct bp_xbox_rumble *rumble);
/* Named profiles share native command encoding and setup. LIGHT takes packed
 * HSV; DRIVE takes power/speed and steering percentages; SOUND takes a
 * bp_lwp3_sound. Unused auxiliary values must be zero. Like write, commands
 * complete through get(). Unsupported profile/command pairs have no effects. */
__syscall int bp_lwp3_command(uint32_t handle, uint32_t command, int32_t value, int32_t auxiliary);
__syscall int bp_lwp3_palette_get(uint32_t handle, struct bp_sensor_palette *palette);
__syscall int bp_lwp3_palette_set(uint32_t handle, const struct bp_sensor_palette *palette);
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
/* Copy the charger's cached observations without I/O. Check state.error before
 * using current/status; a driver fault is distinct from stale/failed sensing. */
__syscall int bp_charger_get(struct bp_charger_state *state);

/* Copy port identity and raw UART data; no I/O or port reconfiguration.
 * Ports are numbered 0..5 (A..F). */
__syscall int bp_port_get(uint32_t port, struct bp_port_state *state);
/* Claim a coasting port for custom 8N1 UART, without P1/P2 supply or LEGO
 * negotiation. baud must be positive and supported by the Zephyr driver.
 * Configuration is asynchronous: wait for bp_port_get().error != -EAGAIN,
 * matching the returned generation. Release restores electrical detection.
 * Raw mode cannot detect unplugging; generation identifies this lease. */
__syscall int bp_uart_open(uint32_t port, uint32_t baud, uint32_t *generation);
/* Same lease with battery supply on P1/P2: 0 none, 1 P1 positive, 2 P2
 * positive. Nonzero power requires uart/power_allowed, otherwise EACCES before
 * claiming or driving. Clear/baud changes retain supply; release/errors coast. */
__syscall int bp_uart_open_power(uint32_t port, uint32_t baud, uint32_t power_pin, uint32_t *generation);
/* Set permission for new powered UART leases (0/1) in RAM. This is operator
 * acknowledgement, not isolation from an application authorized to drive ports.
 * Existing leases are unaffected. Only config save persists the setting. */
__syscall int bp_uart_allow_power(uint32_t allowed);
/* Queue a new positive baud, or clear RX/TX at the current baud. Both cancel
 * outstanding I/O, retain the lease and complete through bp_port_get().error. */
__syscall int bp_uart_configure(uint32_t port, uint32_t generation, uint32_t baud);
__syscall int bp_uart_clear(uint32_t port, uint32_t generation);
/* Nonblocking count / read, with negative errno on failure. Reads consume up
 * to 128 bytes; no caller buffer is retained. RX capacity is 512 bytes; overrun
 * fails the session explicitly until clear/reconfiguration or release. */
__syscall int bp_uart_waiting(uint32_t port, uint32_t generation);
__syscall int bp_uart_read(uint32_t port, uint32_t generation, uint8_t *data, uint32_t size);
/* Copy 1..32 bytes, one pending write. Match request against port.write;
 * completion includes the last UART stop bit, with no device acknowledgement. */
__syscall int bp_uart_write(uint32_t port, uint32_t generation,
	const uint8_t *data, uint32_t size, uint32_t *request);
/* Read one advertised mode after negotiation. Use generation from bp_port_get;
 * ESTALE means a different connection. No mode change, ownership or device I/O. */
__syscall int bp_port_get_mode(uint32_t port, uint32_t generation, uint32_t mode,
	struct bp_port_mode *info);
/* Claim an idle UART port and select an advertised mode without writing data
 * or driving a motor. Generation guards hotplug; 0 means queued. Wait for
 * bp_port_get().mode_error == 0 and the same generation/selected_mode/mode.
 * Other claims, active motor control and invalid modes fail before changes. */
__syscall int bp_port_select(uint32_t port, uint32_t generation, uint32_t mode);
/* Select and write a writable UART mode. Copies exactly its advertised logical
 * payload (1..32 bytes) before returning 0; request receives the completion
 * token for bp_port_get().write. One pending write per port, never overwritten. */
__syscall int bp_port_write(uint32_t port, uint32_t generation, uint32_t mode,
	const uint8_t *data, uint32_t size, uint32_t *request);
/* Request fresh electrical/UART negotiation of the same device type. Claims
 * an idle port and invalidates its previous generation immediately. Observe
 * bp_port_get().reset using the returned token; release cancels the request. */
__syscall int bp_port_reset(uint32_t port, uint32_t generation, uint32_t *request);
/* Shared converted measurement. Inspect error, fields, sample_ms and generation.
 * Reads never change modes or interfere with a running application. */
__syscall int bp_sensor_get(uint32_t port, struct bp_sensor_sample *sample);
/* Claim this sensor's measurement mode until release/app exit. Nonblocking:
 * 0 means accepted; get() reports EAGAIN until a settled sample is available.
 * Unsupported measurements return ENOTSUP before a command or claim. */
__syscall int bp_sensor_configure(uint32_t port, uint32_t measurement);
/* Bind selection to an inspected connection; stale generations fail before
 * ownership or mode changes. Device-class bindings use this form. */
__syscall int bp_sensor_select(uint32_t port, uint32_t generation, uint32_t measurement);
/* Per-port color palette. get() is observational; set() claims the current
 * generation and copies a validated replacement, or resets defaults for NULL.
 * The palette survives release/hotplug; Python color constructors reset it. */
__syscall int bp_sensor_palette_get(uint32_t port, uint32_t generation, struct bp_sensor_palette *palette);
__syscall int bp_sensor_palette_set(uint32_t port, uint32_t generation, const struct bp_sensor_palette *palette);
/* Queues a supported sensor's lights using the ordinary port write result.
 * Unsupported device/format and invalid values fail before selecting a mode. */
__syscall int bp_sensor_light_write(uint32_t port, uint32_t generation,
	const struct bp_sensor_light *light, uint32_t *request);
/* Powered Up 88005 light: 0..1000 permille, zero coasts. Completion is reported
 * by bp_motor_get using the returned request and the same generation. */
__syscall int bp_port_light_write(uint32_t port, uint32_t generation, uint32_t brightness, uint32_t *request);
/* 3x3 ColorLightMatrix: nine copied packed HSV colors, or NULL for off.
 * Completion follows the ordinary UART port write contract. */
__syscall int bp_port_matrix_write(uint32_t port, uint32_t generation, const uint32_t *colors, uint32_t *request);
/* Queues a copied Power Functions command; completion means UART transmission
 * plus 250 ms settling, never receiver acknowledgement. Release sends coast
 * to every addressed output before relinquishing the port claim. */
__syscall int bp_port_pf_write(uint32_t port, uint32_t generation,
	const struct bp_pf_command *command, uint32_t *request);
/* Release the port claim: coast its motor and restore automatic measurement.
 * Accepted asynchronously; ownership remains until coast succeeds and pending
 * UART output is cancelled or finishes transmission and settling.
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
/* Configure a connected DC motor with direction +1/-1 and its model's voltage
 * limit, queueing coast first. These connection-bound operations use the same
 * owner/worker as direct native drive. Match request with motor_get().completed;
 * wait for setup completion before settings or drive. No driver I/O in caller. */
__syscall int bp_motor_setup(uint32_t port, uint32_t generation, int32_t direction, uint32_t *request);
/* Duty is -1000..1000 in configured direction. Convert using cached battery
 * voltage and clamp to the configured limit. coast=1 requires duty=0; duty=0
 * with coast=0 brakes. Both stop modes work without a battery observation. */
__syscall int bp_motor_dc(uint32_t port, uint32_t generation, int32_t duty, uint32_t coast, uint32_t *request);
/* 0..model maximum mV; applies to subsequent DC commands, without flash I/O. */
__syscall int bp_motor_set_voltage(uint32_t port, uint32_t generation, int32_t max_voltage);
/* Connection-bound asynchronous coast/release. Stale objects cannot release
 * a replacement device. Completion is unowned + valid coast, or cleanup_error. */
__syscall int bp_motor_close(uint32_t port, uint32_t generation);
/* Claim a negotiated encoder motor, coast and start continuous observation.
 * A regulated command also starts observation. Coast/brake/DC retain it;
 * release, disconnect or observation failure stop it. Match request completion. */
__syscall int bp_motor_observe(uint32_t port, uint32_t generation, uint32_t *request);
/* Snapshot only, no I/O. window_ms rounds to the nearest 5 ms (3..302 ms).
 * Inspect error; stale/absent observations never masquerade as zero speed. */
__syscall int bp_motor_observation_get(uint32_t port, uint32_t generation, uint32_t window_ms,
	struct bp_motor_observation *value);
/* Queue a regulated speed/position command; does not wait for motion. The
 * command is copied. bp_motor_get reports driver completion separately from
 * trajectory completion. Coast or release cancels regulation. */
__syscall int bp_motor_control(uint32_t port, const struct bp_motor_command *command);

/* Configure the shared servo view, coast and initialize the observer. Commands
 * return a motor request token; bp_motor_get reports driver completion. */
__syscall int bp_servo_setup(uint32_t port, uint32_t generation,
	const struct bp_servo_setup *setup, uint32_t *request);
__syscall int bp_servo_get(uint32_t port, uint32_t generation, uint32_t window_ms,
	struct bp_servo_state *value);
/* Explicit angle is in output degrees. Absolute reset ignores angle and needs
 * a shaft index. Active control stops with its completion mode; direct DC drive
 * is retained. The observer's history resets, not its control settings. */
__syscall int bp_servo_reset(uint32_t port, uint32_t generation, int32_t angle,
	uint32_t absolute, uint32_t *request);
/* Generation-bound motion in configured output coordinates. Application speed
 * is capped at the model limit. Zero speed in ANGLE/TARGET completes in place;
 * TIME with zero duration completes rather than starting an infinite run. */
__syscall int bp_servo_move(uint32_t port, uint32_t generation,
	const struct bp_servo_command *command, uint32_t *request);
/* Set uses the revision and scale from get; no partial update on invalid input.
 * It does not replace a movement. Poll applied to observe the control frame. */
__syscall int bp_servo_settings_get(uint32_t port, uint32_t generation, struct bp_servo_settings *value);
__syscall int bp_servo_settings_set(uint32_t port, uint32_t generation, uint32_t group,
	const struct bp_servo_settings *value, uint32_t *revision);
__syscall int bp_servo_trajectory_get(uint32_t port, uint32_t generation, struct bp_servo_trajectory *value);
__syscall int bp_servo_model_get(uint32_t port, uint32_t generation, struct bp_motor_model_state *value);
/* Motor logs own firmware RAM until restart or application cleanup. Rows are
 * immutable once published; a changed revision rejects an interrupted export. */
__syscall int bp_motor_log_start(uint32_t port, uint32_t generation, uint32_t kind, uint32_t duration_ms, uint32_t down_sample);
__syscall int bp_motor_log_status(uint32_t port, uint32_t generation, uint32_t kind, uint32_t stop, struct bp_motor_log_status *value);
__syscall int bp_motor_log_read(uint32_t port, uint32_t generation, uint32_t kind, uint32_t revision, uint32_t row,
	struct bp_motor_log_row *value);

/* Pair two configured servos. Open queues origin/settings initialization as
 * request 1; command/settings return later tokens. Completion means both
 * outputs accepted, not motion finished: get().done reports that separately.
 * A direct Motor command cancels paired motion but preserves the handle.
 * Closing either Motor or the pair invalidates it. Close is asynchronous;
 * get returns ENODEV once the worker has retired it. Gyro uses the shared IMU
 * heading; reset also resets that heading. A lost IMU epoch stops the pair. */
__syscall int bp_drivebase_open(const struct bp_drivebase_setup *setup, uint32_t *handle);
__syscall int bp_drivebase_get(uint32_t handle, struct bp_drivebase_state *value);
__syscall int bp_drivebase_command(uint32_t handle, const struct bp_drivebase_command *command, uint32_t *request);
__syscall int bp_drivebase_settings_set(uint32_t handle, const struct bp_drivebase_settings *settings, uint32_t *request);
__syscall int bp_drivebase_close(uint32_t handle);
__syscall int bp_drivebase_axis_get(uint32_t handle, uint32_t axis, struct bp_drivebase_axis_state *value);
__syscall int bp_drivebase_axis_set(uint32_t handle, uint32_t axis, uint32_t group, const struct bp_servo_settings *value, uint32_t *request);
__syscall int bp_drivebase_log_start(uint32_t handle, uint32_t axis, uint32_t duration_ms, uint32_t down_sample);
__syscall int bp_drivebase_log_status(uint32_t handle, uint32_t axis, uint32_t stop, struct bp_motor_log_status *value);
__syscall int bp_drivebase_log_read(uint32_t handle, uint32_t axis, uint32_t revision, uint32_t row, struct bp_motor_log_row *value);


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

/* Read cached IMU, battery and selected encoder/output publications. No I/O,
 * allocation, mode changes or ownership. Returns 0 even when individual sources
 * have errors; inspect their error fields. Bits 0..5 select A..F; zero is valid.
 * Retained values are never relabeled as fresh. No application pointers persist. */
__syscall int bp_control_frame_get(uint32_t port_mask, struct bp_control_frame *frame);

/* One live raw-output group per hub. Validate the entire group before accepting
 * any output/claim. Replacing a pending group or changing a live mask is EBUSY.
 * Fault, expiry or a coast/release of a member cancels and coasts the whole group.
 * Other individual drive commands cannot interleave with a live group.
 */
__syscall int bp_motor_group_set(const struct bp_motor_group_command *command, uint32_t *request);
__syscall int bp_motor_group_get(struct bp_motor_group_state *state);

/* Configure once per application run, before periodic work. The supervisor
 * allocates the bounded recording while this caller waits. Exit/stop freezes
 * its tail for host retrieval; replacement or explicit host clear discards it.
 * Host disconnect neither stops nor clears recording. */
__syscall int bp_record_begin(const struct bp_record_spec *spec);
/* Copy one complete record, without allocation, waiting or transport work.
 * Full buffers overwrite the oldest record; host cursors expose every gap.
 * Returns 0 on acceptance, ENODATA before begin, EMSGSIZE for a wrong length. */
__syscall int bp_record_write(const void *data, size_t length);

#include <zephyr/syscalls/brickphyr.h>
#endif

#include <brickphyr_api.h>

/* The firmware calls this entry in user mode; return ends the application. */
int app_main(void);
