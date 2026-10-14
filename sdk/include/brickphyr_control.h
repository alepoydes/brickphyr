// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <brickphyr_imu.h>
#include <brickphyr_battery.h>
#include <brickphyr_port.h>

/* Periodic native release. Times use the same monotonic clock as input frames.
 * skipped counts omitted releases since the previous call, not wakeup jitter.
 * The copied parameter block and its revision belong to this exact step. */
struct bp_control_step {
	int64_t scheduled_us, started_us;
	uint32_t sequence, skipped, parameter_revision, parameter_size;
};

/* One port publication, with explicit connection identity for each output.
 * Encoder coordinates are physical shaft millidegrees. sample_ms is parser
 * completion, not the sensor's physical sampling instant. Check error first.
 * Output has its own error/validity; applied_ms means driver completion, not
 * measured voltage. A pending command does not replace the reported output.
 * An output from an older generation reports ESTALE and valid=0.
 */
struct bp_control_port {
	int64_t sample_ms, angle_mdeg;
	int64_t requested_ms, applied_ms;
	uint32_t generation, sequence, request, completed;
	uint32_t request_generation, output_generation;
	int32_t error, output_error, cleanup_error, duty_permille;
	uint8_t mode, valid, pending, owner;
};

/* Read-only input for one control step. Sources retain their own times and
 * errors; capture_start_us/end_us timestamp this read, not the physical samples.
 * capture_resolution_us reports their quantization (1 us on F4; system-tick
 * resolution on targets without a 64-bit hardware cycle counter).
 * Each source is coherent, but they are not a simultaneous hardware latch.
 * Only ports selected by port_mask are populated; the others are zero.
 * Compare IMU epoch and port generations before integrating across frames.
 */
struct bp_control_frame {
	int64_t capture_start_us, capture_end_us;
	uint32_t port_mask, capture_resolution_us;
	struct bp_imu_state imu;
	struct bp_battery_sample battery;
	struct bp_control_port ports[BP_PORT_COUNT];
};

/* One bounded raw-output command. Duty is physical signed permille, without
 * direction/gearing/voltage conversion; zero duty brakes. coast_mask selects
 * passive outputs and must be a subset of port_mask. Non-coasting commands
 * require an absolute monotonic deadline_ms, not a duration after application.
 */
struct bp_motor_group_command {
	int64_t deadline_ms;
	uint32_t port_mask, coast_mask;
	struct {
		uint32_t generation;
		int32_t duty_permille;
	} ports[BP_PORT_COUNT];
};

/* Last accepted group, retained until replacement. pending includes output
 * writes and fault cleanup; active means the group still commands a drive.
 * applied_mask records successful original writes, even if later coasted.
 * error and cleanup_error preserve the first cause and failed cleanup separately.
 * Per-port requests identify the original writes in bp_control_frame_get().
 */
struct bp_motor_group_state {
	int64_t requested_ms, completed_ms, deadline_ms;
	uint32_t request, port_mask, applied_mask;
	int32_t error, cleanup_error;
	uint8_t pending, active;
	struct {
		uint32_t generation, request;
		int32_t error, cleanup_error;
	} ports[BP_PORT_COUNT];
};

/* Previous group state at admission of a control step's next command. A
 * successful call binds request to the current control step. previous_sequence
 * identifies the step which issued previous.request through this API; zero
 * means no known association in this native context. previous.request==0 means
 * no previous snapshot. A rejected command leaves request==0 and the existing
 * association unchanged. The previous result may still be pending on rejection.
 */
struct bp_control_motor_result {
	uint32_t request, previous_sequence;
	struct bp_motor_group_state previous;
};
