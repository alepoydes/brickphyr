// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control.h"

enum drivebase_axis { DRIVEBASE_DISTANCE, DRIVEBASE_HEADING, DRIVEBASE_AXES };

/* Private computation state. The port worker supplies both directed shaft
 * states from one frame, and optionally replaces heading position/speed with
 * the scaled IMU measurement. No device, clock, allocation or ownership here. */
struct drivebase_control {
	pbio_control_t control[DRIVEBASE_AXES];
	pbio_angle_t offset[DRIVEBASE_AXES];
	bool paused;
};

struct drivebase_state { pbio_control_state_t axis[DRIVEBASE_AXES]; };
struct drivebase_settings {
	int32_t speed, acceleration, deceleration;
	int32_t turn_rate, turn_acceleration, turn_deceleration;
};
enum drivebase_motion { DRIVEBASE_RELATIVE, DRIVEBASE_ABSOLUTE_TURN, DRIVEBASE_TIMED };
struct drivebase_command {
	int32_t distance, angle, speed, turn_rate;
	uint32_t duration_ms;
	enum drivebase_motion motion;
	pbio_control_on_completion_t finish;
};

struct drivebase_reference {
	pbio_trajectory_reference_t axis[DRIVEBASE_AXES];
	int32_t torque[DRIVEBASE_AXES];
	pbio_dcmotor_actuation_t actuation;
	bool active, done, stalled;
};

/* Input settings use physical shaft units. Gear scale is shaft mdeg per output
 * degree; dimensions are micrometres. A SpikeBase uses scale 1000 on both axes. */
int drivebase_control_init(struct drivebase_control *db,
	const pbio_control_settings_t *left, const pbio_control_settings_t *right,
	int32_t gear_scale, int32_t wheel_um, int32_t track_um, bool spike);
void drivebase_control_state(const struct drivebase_control *db,
	const pbio_control_state_t *left, const pbio_control_state_t *right,
	struct drivebase_state *state);
/* Reset needs the encoder state before any optional gyro override. The owner
 * also resets shared IMU heading when the gyro is selected. */
int drivebase_control_reset(struct drivebase_control *db,
	const struct drivebase_state *state, int32_t distance, int32_t angle);
void drivebase_control_stop(struct drivebase_control *db);
bool drivebase_control_active(const struct drivebase_control *db);
bool drivebase_control_done(const struct drivebase_control *db);
int drivebase_control_command(struct drivebase_control *db, uint32_t ticks,
	const struct drivebase_state *state, const struct drivebase_command *command);
int drivebase_control_step(struct drivebase_control *db, uint32_t ticks,
	const struct drivebase_state *state, struct drivebase_reference *value);
/* Mix distance/heading feedback and reference feed-forward into one directed
 * wheel's shaft voltage. Direction is applied by the port boundary afterwards. */
void drivebase_control_output(const struct drivebase_reference *value,
	const struct motor_control *motor, bool right, struct motor_reference *out);
void drivebase_settings_get(const struct drivebase_control *db, struct drivebase_settings *value);
int drivebase_settings_set(struct drivebase_control *db, const struct drivebase_settings *value);
int drivebase_arc_command(int32_t radius, int32_t amount, bool distance,
	bool legacy_curve, struct drivebase_command *command);
int drivebase_tank_command(int32_t left, int32_t right, int32_t angle,
	bool timed, struct drivebase_command *command);
int drivebase_steering_to_tank(int32_t speed, int32_t steering, int32_t *left, int32_t *right);
