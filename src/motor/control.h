// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <brickphyr_motor.h>
#include <pbio/control.h>
#include <pbio/observer.h>

/* Private computation state, owned by the port worker. No device I/O, storage,
 * application buffers or scheduler. Instances must not move after initialization
 * because PBIO integrators refer to the contained settings. */
struct motor_control {
	pbio_control_t control;
	pbio_observer_t observer;
	int32_t max_voltage;
	bool timed_hold;
};

struct motor_reference {
	int64_t angle_mdeg;
	int32_t speed_mdeg_s, acceleration_mdeg_s2, estimated_speed;
	int32_t voltage_mv;
	bool coast, active, done, stalled;
};

struct motor_observation {
	int32_t estimated_speed, load_mnm;
	bool stalled;
};

int motor_control_init(struct motor_control *motor, uint16_t id, int64_t angle_mdeg);
int motor_control_init_profile(struct motor_control *motor, uint16_t id, int64_t angle_mdeg, int32_t profile);
int motor_control_reset_angle(struct motor_control *motor, int64_t angle_mdeg);
void motor_control_settings_get(const pbio_control_settings_t *settings, int32_t scale,
	struct bp_servo_settings *value);
int motor_control_settings_set(pbio_control_settings_t *settings, uint32_t group,
	const struct bp_servo_settings *value);
void motor_control_settings_apply(struct motor_control *motor, const pbio_control_settings_t *settings);
void motor_control_trajectory_get(struct motor_control *motor, struct bp_servo_trajectory *value);
int motor_control_check(uint16_t id, int64_t angle, const struct bp_motor_command *command);
int32_t motor_control_speed_limit(uint16_t id);
int motor_control_command(struct motor_control *motor, uint32_t ticks, int64_t angle_mdeg,
	const struct bp_motor_command *command, int32_t battery_mv);
int motor_control_run(struct motor_control *motor, uint32_t time_ticks, int64_t angle_mdeg,
	int32_t speed_mdeg_s, uint32_t duration_ms, pbio_control_on_completion_t finish);
int motor_control_target(struct motor_control *motor, uint32_t time_ticks, int64_t angle_mdeg,
	int64_t target_mdeg, int32_t speed_mdeg_s, pbio_control_on_completion_t finish);
/* Call once per PBIO_CONFIG_CONTROL_LOOP_TIME_MS. The owner checks acquisition
 * validity and missed periods before calling. A rejected step never actuates. */
int motor_control_step(struct motor_control *motor, uint32_t time_ticks, int64_t angle_mdeg,
	struct motor_reference *result);
/* Predict the next state using the successfully applied voltage, after step.
 * This is separate from requesting voltage: a failed PWM write is not applied. */
int motor_control_predict(struct motor_control *motor, uint32_t time_ticks,
	int64_t angle_mdeg, bool coast, int32_t voltage_mv);
/* Read the already predicted observer state. Does not advance it again. */
int motor_control_observe(struct motor_control *motor, uint32_t time_ticks,
	int64_t angle_mdeg, bool coast, struct motor_observation *result);
