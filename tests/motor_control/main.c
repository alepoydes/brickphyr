// SPDX-License-Identifier: Apache-2.0
#include "control.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pbio/differentiator.h>
#include <zephyr/ztest.h>

ZTEST(motor_control, test_models_and_rejection)
{
	const uint16_t ids[] = {38, 46, 47, 48, 49, 65, 75, 76};
	struct motor_control motor;
	for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
		zassert_ok(motor_control_init(&motor, ids[i], 0));
		zassert_ok(motor_control_run(&motor, 0, 0, 300000, 1000, PBIO_CONTROL_ON_COMPLETION_COAST));
		struct motor_reference out;
		zassert_ok(motor_control_step(&motor, 0, 0, &out));
		zassert_true(out.active);
		zassert_equal(out.angle_mdeg, 0);
		zassert_equal(out.speed_mdeg_s, 0);
		zassert_equal(out.acceleration_mdeg_s2, 2000000);
		zassert_true(out.voltage_mv > 0 && out.voltage_mv <= (ids[i] == 65 ? 6000 : 9000));
	}
	zassert_equal(motor_control_init(&motor, 201, 0), -ENOTSUP);
	zassert_equal(motor_control_init(&motor, 48, INT64_MAX), -ERANGE);
	zassert_equal(motor_control_run(&motor, 0, 0, INT32_MIN, 1, PBIO_CONTROL_ON_COMPLETION_COAST), -EINVAL);
	zassert_equal(motor_control_run(&motor, 0, 0, 10000, UINT32_MAX, PBIO_CONTROL_ON_COMPLETION_COAST), -EINVAL);
	zassert_equal(motor_control_target(&motor, 0, 0, INT64_MAX, 10000, PBIO_CONTROL_ON_COMPLETION_HOLD), -ERANGE);
	zassert_equal(motor_control_target(&motor, 0, 0, 360000, -10000, PBIO_CONTROL_ON_COMPLETION_HOLD), -EINVAL);
	zassert_equal(motor_control_predict(&motor, 0, 0, true, 1), -EINVAL);
	struct motor_reference out;
	zassert_ok(motor_control_step(&motor, 50, 0, &out));
	zassert_true(out.active, "Rejected request stopped previous control");
	zassert_true(out.speed_mdeg_s > 0);
	zassert_equal(motor_control_step(&motor, 100, INT64_MIN, &out), -ERANGE);
}

/* A deliberately simple, independent synthetic plant, not the PBIO observer
 * model: a 40ms first-order speed response, 1000deg/s at9V. It checks closed
 * feedback and command transitions, not any real motor's tuning or latency. */
static void advance(struct motor_control *motor, uint32_t ticks, double *angle,
	double *speed, struct motor_reference *out)
{
	int64_t measured = (int64_t)*angle;
	zassert_ok(motor_control_step(motor, ticks, measured, out));
	zassert_true(abs(out->voltage_mv) <= motor->max_voltage);
	zassert_ok(motor_control_predict(motor, ticks, measured, out->coast, out->voltage_mv));
	*speed += (out->voltage_mv * (1000000.0 / 9000) - *speed) * (0.005 / 0.040);
	*angle += *speed * 0.005;
}

ZTEST(motor_control, test_target_and_hold)
{
	for (int direction = -1; direction <= 1; direction += 2) {
		struct motor_control motor;
		/* Exceeds int32 millidegrees; the target must retain its long origin. */
		double angle = (int64_t)direction * 3000000000LL, speed = 0;
		int64_t target = (int64_t)angle + direction * 360000;
		zassert_ok(motor_control_init(&motor, 48, angle));
		zassert_ok(motor_control_target(&motor, 0, angle, target, 360000, PBIO_CONTROL_ON_COMPLETION_HOLD));
		struct motor_reference out;
		for (uint32_t ticks = 0; ticks < 30000; ticks += 50) advance(&motor, ticks, &angle, &speed, &out);
		zassert_true(llabs((int64_t)angle - target) <= 11000, "target error %lld", (long long)((int64_t)angle - target));
		zassert_equal(out.angle_mdeg, target);
		zassert_true(out.done && out.active && !out.coast);
		/* A displaced shaft must receive opposing feedback while holding. */
		angle += 30000;
		advance(&motor, 30000, &angle, &speed, &out);
		zassert_true(out.voltage_mv < 0);
	}
}

ZTEST(motor_control, test_timed_completion_and_wrap)
{
	for (int finish = PBIO_CONTROL_ON_COMPLETION_COAST; finish <= PBIO_CONTROL_ON_COMPLETION_HOLD; finish++) {
		struct motor_control motor;
		double angle = 3000000000LL, speed = 0;
		uint32_t start = UINT32_MAX - 999;
		zassert_ok(motor_control_init(&motor, 48, angle));
		zassert_ok(motor_control_run(&motor, start, angle, 300000, 500, finish));
		struct motor_reference out;
		uint32_t elapsed;
		for (elapsed = 0; elapsed <= 10000; elapsed += 50) {
			advance(&motor, start + elapsed, &angle, &speed, &out);
			if (out.done) break;
		}
		zassert_equal(elapsed, 5000);
		zassert_equal(out.active, finish == PBIO_CONTROL_ON_COMPLETION_HOLD);
		zassert_equal(out.coast, finish == PBIO_CONTROL_ON_COMPLETION_COAST);
		if (out.active) {
			int64_t stop_angle = angle;
			for (elapsed += 50; elapsed < 20000; elapsed += 50) {
				advance(&motor, start + elapsed, &angle, &speed, &out);
			}
			zassert_true(llabs((int64_t)angle - stop_angle) < 11000);
			zassert_true(out.angle_mdeg > 3000000000LL, "Timed hold narrowed the angle");
		}
	}
}

ZTEST(motor_control, test_rejected_trajectory_preserves_motion)
{
	struct motor_control motor;
	zassert_ok(motor_control_init(&motor, 48, 0));
	zassert_ok(motor_control_run(&motor, 0, 0, 300000, 1000, PBIO_CONTROL_ON_COMPLETION_COAST));
	/* The angle is representable, but this move exceeds the planner's time
	 * range. Rejection must not corrupt the running acceleration ramp. */
	zassert_equal(motor_control_target(&motor, 0, 0, 1000000000, 1000,
		PBIO_CONTROL_ON_COMPLETION_HOLD), -ERANGE);
	struct motor_reference out;
	zassert_ok(motor_control_step(&motor, 50, 0, &out));
	zassert_true(out.active && out.speed_mdeg_s > 0);
	zassert_equal(out.acceleration_mdeg_s2, 2000000);
}

ZTEST(motor_control, test_subresolution_position_speed)
{
	struct motor_control motor;
	zassert_ok(motor_control_init(&motor, 48, 0));
	zassert_equal(motor_control_target(&motor, 0, 0, 360000, 1,
		PBIO_CONTROL_ON_COMPLETION_HOLD), -ERANGE);
}

ZTEST(motor_control, test_passive_observation)
{
	struct motor_control motor;
	struct motor_observation observation;
	zassert_ok(motor_control_init(&motor, 48, 0));
	/* Independent shaft movement: 1 degree per 5 ms gives 200 deg/s. Reading
	 * the observation each tick must not advance the differentiator twice. */
	for (uint32_t sample = 1; sample <= 30; sample++) {
		uint32_t ticks = sample * 50;
		int64_t angle = sample * 1000;
		zassert_ok(motor_control_predict(&motor, ticks, angle, true, 0));
		zassert_ok(motor_control_observe(&motor, ticks, angle, true, &observation));
		zassert_equal(observation.load_mnm, 0);
		zassert_false(observation.stalled);
	}
	int32_t speed;
	zassert_ok(pbio_differentiator_get_speed(&motor.observer.differentiator, 100, &speed));
	zassert_equal(speed, 200000);
	zassert_true(observation.estimated_speed > 0);

	/* Voltage on a fixed shaft produces opposing load and sustained stall.
	 * Coast then clears both, without a second observer update per frame. */
	zassert_ok(motor_control_init(&motor, 48, 0));
	for (uint32_t sample = 0; sample <= 200; sample++) {
		uint32_t ticks = sample * 50;
		zassert_ok(motor_control_predict(&motor, ticks, 0, false, 9000));
		zassert_ok(motor_control_observe(&motor, ticks, 0, false, &observation));
	}
	zassert_true(observation.load_mnm < 0);
	zassert_true(observation.stalled);
	zassert_ok(motor_control_predict(&motor, 10050, 0, true, 0));
	zassert_ok(motor_control_observe(&motor, 10050, 0, true, &observation));
	zassert_equal(observation.load_mnm, 0);
	zassert_false(observation.stalled);
}

ZTEST(motor_control, test_servo_motion_commands)
{
	struct motor_control motor;
	const int64_t origin = 3000000000LL;
	zassert_ok(motor_control_init_profile(&motor, 48, origin, 6));
	zassert_equal(motor.control.settings.position_tolerance, 6000);
	struct bp_motor_command command = {.mode = BP_MOTOR_SPEED, .action = BP_MOTOR_TIMED,
		.speed_mdeg_s = 100000, .finish = BP_MOTOR_FINISH_HOLD};
	zassert_ok(motor_control_check(48, origin, &command));
	zassert_ok(motor_control_command(&motor, 0, origin, &command, 9000));
	struct motor_reference out;
	zassert_ok(motor_control_step(&motor, 0, origin, &out));
	zassert_true(out.done && out.active, "Zero time became an infinite run");
	zassert_ok(motor_control_step(&motor, 50, origin, &out));
	zassert_equal(out.angle_mdeg, origin);

	/* Tracking replaces the reference immediately, without narrowing the long
	 * origin. Reset clears speed history while retaining the selected profile. */
	command = (struct bp_motor_command){.mode = BP_MOTOR_POSITION, .action = BP_MOTOR_TRACK,
		.target_mdeg = origin - 30000, .finish = BP_MOTOR_FINISH_HOLD};
	zassert_ok(motor_control_command(&motor, 100, origin, &command, 9000));
	zassert_ok(motor_control_step(&motor, 100, origin, &out));
	zassert_equal(out.angle_mdeg, origin - 30000);
	zassert_true(out.voltage_mv < 0);
	zassert_ok(motor_control_reset_angle(&motor, origin));
	zassert_equal(motor.control.settings.position_tolerance, 6000);
	int32_t speed;
	zassert_ok(pbio_differentiator_get_speed(&motor.observer.differentiator, 100, &speed));
	zassert_equal(speed, 0);

	/* A fixed synthetic shaft completes a stall objective, including HOLD at
	 * positions which cannot be represented as int32 millidegrees. */
	command = (struct bp_motor_command){.mode = BP_MOTOR_SPEED, .action = BP_MOTOR_UNTIL_STALLED,
		.speed_mdeg_s = 500000, .finish = BP_MOTOR_FINISH_HOLD, .duty_limit = 500};
	zassert_ok(motor_control_command(&motor, 150, origin, &command, 9000));
	for (uint32_t ticks = 150; ticks < 30000; ticks += 50) {
		zassert_ok(motor_control_step(&motor, ticks, origin, &out));
		zassert_ok(motor_control_predict(&motor, ticks, origin, out.coast, out.voltage_mv));
		if (out.done) break;
	}
	zassert_true(out.done && out.active);
	zassert_ok(motor_control_step(&motor, 30000, origin, &out));
	zassert_equal(out.angle_mdeg, origin);
	zassert_ok(motor_control_reset_angle(&motor, origin));
	command.finish = BP_MOTOR_FINISH_COAST_SMART;
	zassert_ok(motor_control_command(&motor, 0, origin, &command, 9000));
	for (uint32_t time = 0; time < 30000; time += 50) {
		zassert_ok(motor_control_step(&motor, time, origin, &out));
		zassert_ok(motor_control_predict(&motor, time, origin, out.coast, out.voltage_mv));
		if (out.done) break;
	}
	zassert_true(out.done && !out.active && out.coast, "Smart stall completion kept driving");

	/* Two relative commands reuse PBIO's smart endpoint. A deliberately simple
	 * plant exercises actual feedback/completion instead of a copied planner. */
	double angle = origin, velocity = 0;
	zassert_ok(motor_control_init(&motor, 48, origin));
	command = (struct bp_motor_command){.mode = BP_MOTOR_POSITION, .action = BP_MOTOR_RELATIVE,
		.target_mdeg = -120000, .speed_mdeg_s = 240000, .finish = BP_MOTOR_FINISH_COAST_SMART};
	uint32_t ticks = 0;
	for (int move = 1; move <= 2; move++) {
		zassert_ok(motor_control_command(&motor, ticks, angle, &command, 9000));
		uint32_t end = ticks + 30000;
		do {
			advance(&motor, ticks, &angle, &velocity, &out);
			ticks += 50;
		} while (!out.done && ticks < end);
		zassert_true(out.done);
		zassert_equal(out.angle_mdeg, origin - move * 120000);
		zassert_true(llabs((int64_t)angle - out.angle_mdeg) <= 11000);
	}
}

ZTEST(motor_control, test_settings_during_motion)
{
	struct motor_control motor;
	zassert_ok(motor_control_init(&motor, 48, 0));
	struct bp_servo_settings shaft, geared, change, actual;
	motor_control_settings_get(&motor.control.settings, 1000, &shaft);
	motor_control_settings_get(&motor.control.settings, 5000, &geared);
	zassert_equal(shaft.speed, 1000);
	zassert_equal(geared.speed, 200);
	zassert_equal(shaft.acceleration, geared.acceleration * 5);

	pbio_control_settings_t settings = motor.control.settings;
	pbio_control_settings_t original = settings;
	change = geared;
	change.speed = 100;
	change.acceleration = 120;
	change.deceleration = 80;
	change.torque = INT32_MAX;
	zassert_equal(motor_control_settings_set(&settings, BP_SERVO_LIMITS, &change), -EINVAL);
	zassert_equal(memcmp(&settings, &original, sizeof(settings)), 0,
		"Invalid torque partially changed trajectory limits");
	change.torque = geared.torque;
	zassert_ok(motor_control_settings_set(&settings, BP_SERVO_LIMITS, &change));
	motor_control_settings_get(&settings, 5000, &actual);
	zassert_equal(actual.speed, 100);
	zassert_equal(actual.acceleration, 120);
	zassert_equal(actual.deceleration, 80);

	/* A running stall-limited maneuver keeps its PBIO trajectory and temporary
	 * torque cap when shared settings change. The next command uses new limits. */
	struct bp_motor_command command = {.mode = BP_MOTOR_SPEED,
		.action = BP_MOTOR_UNTIL_STALLED, .speed_mdeg_s = 300000,
		.finish = BP_MOTOR_FINISH_COAST, .duty_limit = 250};
	zassert_ok(motor_control_command(&motor, 0, 0, &command, 9000));
	struct motor_reference out;
	zassert_ok(motor_control_step(&motor, 0, 0, &out));
	zassert_ok(motor_control_predict(&motor, 0, 0, out.coast, out.voltage_mv));
	int32_t temporary = motor.control.settings.actuation_max_temporary;
	zassert_true(temporary < motor.control.settings.actuation_max);
	struct bp_servo_trajectory before, after;
	motor_control_trajectory_get(&motor, &before);
	motor_control_settings_apply(&motor, &settings);
	motor_control_trajectory_get(&motor, &after);
	zassert_true(after.active);
	zassert_equal(memcmp(before.points, after.points, sizeof(before.points)), 0);
	zassert_equal(motor.control.settings.actuation_max_temporary, temporary);
	zassert_ok(motor_control_step(&motor, 50, 0, &out));
	zassert_true(out.active);

	pbio_control_stop(&motor.control);
	zassert_ok(motor_control_run(&motor, 100, 0, 500000, 1000, PBIO_CONTROL_ON_COMPLETION_COAST));
	zassert_equal(motor.control.settings.actuation_max_temporary, motor.control.settings.actuation_max);
	zassert_ok(motor_control_step(&motor, 100, 0, &out));
	zassert_equal(out.acceleration_mdeg_s2, 600000);
	motor_control_trajectory_get(&motor, &after);
	zassert_true(after.active);
	zassert_equal(after.points[11], 600);
	zassert_equal(motor_control_run(&motor, 100, 0, 500001, 1000,
		PBIO_CONTROL_ON_COMPLETION_COAST), -EINVAL);
	zassert_ok(motor_control_step(&motor, 150, 0, &out));
	zassert_true(out.active);
}

ZTEST(motor_control, test_model_settings_and_feedback_guard)
{
	struct motor_control motor;
	zassert_ok(motor_control_init(&motor, 48, 0));
	struct bp_motor_model_settings values;
	motor_model_settings_get(&motor.observer.settings, &values);
	values.gain_low = values.gain_high = 0;
	pbio_observer_settings_t settings;
	zassert_ok(motor_model_settings_set(48, &settings, &values));
	motor.observer.settings = settings;
	zassert_ok(motor_control_predict(&motor, 50, 1000, true, 0));
	zassert_equal(motor.observer.speed, 0);

	zassert_ok(motor_control_init(&motor, 48, 0));
	values.gain_low = 100;
	values.gain_high = 400;
	zassert_ok(motor_model_settings_set(48, &settings, &values));
	motor.observer.settings = settings;
	zassert_ok(motor_control_predict(&motor, 50, 1000, true, 0));
	zassert_true(motor.observer.speed > 0);

	pbio_observer_settings_t previous = settings;
	values.friction_cutoff = 0;
	zassert_equal(motor_model_settings_set(48, &settings, &values), -EINVAL);
	zassert_mem_equal(&settings, &previous, sizeof(settings));
	values.friction_cutoff = INT32_MAX;
	zassert_equal(motor_model_settings_set(48, &settings, &values), -EINVAL);
	zassert_mem_equal(&settings, &previous, sizeof(settings));

	zassert_ok(motor_control_init(&motor, 48, 0));
	values.friction_cutoff = 500;
	values.gain_low = values.gain_threshold = 0;
	values.gain_high = INT32_MAX;
	zassert_ok(motor_model_settings_set(48, &settings, &values));
	motor.observer.settings = settings;
	zassert_equal(motor_control_predict(&motor, 50, 2, true, 0), -ERANGE);
	zassert_equal(motor.observer.speed, 0);
}

ZTEST_SUITE(motor_control, NULL, NULL, NULL, NULL, NULL);
