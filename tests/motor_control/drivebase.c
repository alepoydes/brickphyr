// SPDX-License-Identifier: Apache-2.0
#include "drivebase.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/ztest.h>

/* Two independent, directed shafts. This is the same deliberately simple
 * synthetic plant used by the motor test, not a copy of PBIO's observer. */
struct drive_fixture {
	struct motor_control motor[2];
	struct drivebase_control db;
	double angle[2], speed[2];
};

static struct drive_fixture fixture[2];

static void state_get(struct drive_fixture *f, struct drivebase_state *state)
{
	pbio_control_state_t left, right;
	zassert_ok(motor_control_state_get(&f->motor[0], (int64_t)f->angle[0], &left));
	zassert_ok(motor_control_state_get(&f->motor[1], (int64_t)f->angle[1], &right));
	drivebase_control_state(&f->db, &left, &right, state);
}

static void fixture_init(struct drive_fixture *f, int64_t origin)
{
	memset(f, 0, sizeof(*f));
	for (size_t i = 0; i < 2; i++) {
		f->angle[i] = origin;
		zassert_ok(motor_control_init(&f->motor[i], 48, origin));
	}
	zassert_ok(drivebase_control_init(&f->db, &f->motor[0].control.settings,
		&f->motor[1].control.settings, 1000, 56000, 112000, false));
	struct drivebase_state state;
	state_get(f, &state);
	zassert_ok(drivebase_control_reset(&f->db, &state, 0, 0));
}

static void command(struct drive_fixture *f, uint32_t ticks,
	const struct drivebase_command *request)
{
	struct drivebase_state state;
	state_get(f, &state);
	zassert_ok(drivebase_control_command(&f->db, ticks, &state, request));
}

static void advance(struct drive_fixture *f, uint32_t ticks,
	struct drivebase_reference *reference, struct motor_reference output[2])
{
	struct drivebase_state state;
	state_get(f, &state);
	zassert_ok(drivebase_control_step(&f->db, ticks, &state, reference));
	for (size_t i = 0; i < 2; i++) {
		drivebase_control_output(reference, &f->motor[i], i == 1, &output[i]);
		zassert_true(abs(output[i].voltage_mv) <= f->motor[i].max_voltage);
		zassert_ok(motor_control_predict(&f->motor[i], ticks, (int64_t)f->angle[i],
			output[i].coast, output[i].voltage_mv));
		f->speed[i] += (output[i].voltage_mv * (1000000.0 / 9000) - f->speed[i]) *
			(0.005 / 0.040);
		f->angle[i] += f->speed[i] * 0.005;
	}
}

static void until_done(struct drive_fixture *f, uint32_t *ticks,
	struct drivebase_reference *reference, struct motor_reference output[2])
{
	for (unsigned i = 0; i < 2000; i++) {
		advance(f, *ticks, reference, output);
		*ticks += 50;
		if (reference->done) return;
	}
	zassert_unreachable("Drivebase did not finish within ten simulated seconds");
}

ZTEST(drivebase, test_straight_turn_and_hold_feedback)
{
	struct drive_fixture *f = &fixture[0];
	fixture_init(f, 0);
	struct drivebase_command request = {.motion = DRIVEBASE_RELATIVE,
		.distance = 80, .finish = PBIO_CONTROL_ON_COMPLETION_HOLD};
	command(f, 0, &request);
	uint32_t ticks = 0;
	struct drivebase_reference reference;
	struct motor_reference output[2];
	until_done(f, &ticks, &reference, output);
	zassert_true(reference.active && drivebase_control_done(&f->db));
	zassert_true(f->angle[0] > 120000 && f->angle[1] > 120000);
	zassert_true(llabs((int64_t)(f->angle[0] - f->angle[1])) < 20000);

	double left_before = f->angle[0], right_before = f->angle[1];
	request = (struct drivebase_command){.motion = DRIVEBASE_RELATIVE,
		.angle = 90, .finish = PBIO_CONTROL_ON_COMPLETION_HOLD};
	command(f, ticks, &request);
	until_done(f, &ticks, &reference, output);
	zassert_true(reference.active && drivebase_control_done(&f->db));
	zassert_true(f->angle[0] - left_before > 120000);
	zassert_true(f->angle[1] - right_before < -120000);

	/* Equal displacement of both wheels disturbs distance, not heading. */
	f->angle[0] += 30000;
	f->angle[1] += 30000;
	advance(f, ticks, &reference, output);
	zassert_true(output[0].voltage_mv < 0 && output[1].voltage_mv < 0);
}

ZTEST(drivebase, test_arc_synchronization_and_tank_signs)
{
	struct drive_fixture *f = &fixture[0];
	fixture_init(f, 0);
	struct drivebase_command request = {.finish = PBIO_CONTROL_ON_COMPLETION_HOLD};
	zassert_ok(drivebase_arc_command(150, 90, false, false, &request));
	zassert_true(request.distance > 0 && request.angle > 0);
	command(f, 0, &request);
	/* The distance and heading have unequal natural lengths; their finite
	 * references must finish together, not leave one wheel moving alone. */
	zassert_equal(pbio_trajectory_get_duration(&f->db.control[DRIVEBASE_DISTANCE].trajectory),
		pbio_trajectory_get_duration(&f->db.control[DRIVEBASE_HEADING].trajectory));
	uint32_t ticks = 0;
	struct drivebase_reference reference;
	struct motor_reference output[2];
	until_done(f, &ticks, &reference, output);
	zassert_true(f->angle[0] > f->angle[1] && f->angle[1] > 0);

	request = (struct drivebase_command){.finish = PBIO_CONTROL_ON_COMPLETION_HOLD};
	zassert_ok(drivebase_arc_command(-150, 90, false, false, &request));
	zassert_true(request.distance > 0 && request.angle < 0);
	request = (struct drivebase_command){.finish = PBIO_CONTROL_ON_COMPLETION_HOLD};
	zassert_ok(drivebase_tank_command(100, 100, 90, false, &request));
	zassert_equal(request.distance, 0);
	zassert_true(request.angle < 0);
	int32_t left, right;
	zassert_ok(drivebase_steering_to_tank(200, 100, &left, &right));
	zassert_equal(left, 200);
	zassert_equal(right, -200);
}

ZTEST(drivebase, test_rejected_second_axis_preserves_pair)
{
	struct drive_fixture *trial = &fixture[0], *baseline = &fixture[1];
	fixture_init(trial, 0);
	fixture_init(baseline, 0);
	struct drivebase_command request = {.motion = DRIVEBASE_TIMED,
		.speed = 150, .turn_rate = 90, .duration_ms = 1500,
		.finish = PBIO_CONTROL_ON_COMPLETION_COAST};
	command(trial, 0, &request);
	command(baseline, 0, &request);
	struct drivebase_reference a, b;
	struct motor_reference out_a[2], out_b[2];
	for (uint32_t ticks = 0; ticks < 500; ticks += 50) {
		advance(trial, ticks, &a, out_a);
		advance(baseline, ticks, &b, out_b);
	}
	request = (struct drivebase_command){.motion = DRIVEBASE_RELATIVE,
		.distance = 30, .angle = INT32_MAX,
		.finish = PBIO_CONTROL_ON_COMPLETION_HOLD};
	struct drivebase_state state;
	state_get(trial, &state);
	zassert_not_equal(drivebase_control_command(&trial->db, 500, &state, &request), 0);
	for (uint32_t ticks = 500; ticks < 2000; ticks += 50) {
		advance(trial, ticks, &a, out_a);
		advance(baseline, ticks, &b, out_b);
		for (size_t i = 0; i < 2; i++) {
			zassert_equal(out_a[i].voltage_mv, out_b[i].voltage_mv);
			zassert_equal(a.axis[i].position.rotations, b.axis[i].position.rotations);
			zassert_equal(a.axis[i].position.millidegrees,
				b.axis[i].position.millidegrees);
		}
	}
	zassert_true(a.active && b.active);
}

ZTEST(drivebase, test_large_origin_reset_and_absolute_turn)
{
	struct drive_fixture *f = &fixture[0];
	fixture_init(f, 3000000000LL);
	struct drivebase_state state;
	state_get(f, &state);
	zassert_ok(drivebase_control_reset(&f->db, &state, 123, -45));
	state_get(f, &state);
	zassert_equal(pbio_control_settings_ctl_to_app_long(
		&f->db.control[DRIVEBASE_DISTANCE].settings,
		&state.axis[DRIVEBASE_DISTANCE].position), 123);
	zassert_equal(pbio_control_settings_ctl_to_app_long(
		&f->db.control[DRIVEBASE_HEADING].settings,
		&state.axis[DRIVEBASE_HEADING].position), -45);
	struct drivebase_command request = {.motion = DRIVEBASE_ABSOLUTE_TURN,
		.angle = 0, .finish = PBIO_CONTROL_ON_COMPLETION_HOLD};
	command(f, 0, &request);
	struct drivebase_reference reference;
	struct motor_reference output[2];
	advance(f, 0, &reference, output);
	zassert_true(output[0].voltage_mv > 0 && output[1].voltage_mv < 0);
}

ZTEST(drivebase, test_timed_pair_coast_then_hold)
{
	struct drive_fixture *f = &fixture[0];
	fixture_init(f, 0);
	struct drivebase_command request = {.motion = DRIVEBASE_TIMED,
		.speed = 150, .turn_rate = 60, .duration_ms = 200,
		.finish = PBIO_CONTROL_ON_COMPLETION_COAST};
	command(f, 0, &request);
	uint32_t ticks = 0;
	struct drivebase_reference reference;
	struct motor_reference output[2];
	until_done(f, &ticks, &reference, output);
	zassert_false(reference.active);
	zassert_true(output[0].coast && output[1].coast);
	zassert_equal(output[0].voltage_mv, 0);
	zassert_equal(output[1].voltage_mv, 0);
	zassert_true(drivebase_control_done(&f->db));

	request = (struct drivebase_command){.motion = DRIVEBASE_TIMED,
		.speed = 100, .duration_ms = 200,
		.finish = PBIO_CONTROL_ON_COMPLETION_HOLD};
	command(f, ticks, &request);
	until_done(f, &ticks, &reference, output);
	zassert_true(reference.active && drivebase_control_done(&f->db));
	zassert_false(output[0].coast || output[1].coast);
	f->angle[0] += 30000;
	f->angle[1] += 30000;
	advance(f, ticks, &reference, output);
	zassert_true(output[0].voltage_mv < 0 && output[1].voltage_mv < 0);
}

ZTEST(drivebase, test_rejected_configuration_and_conversion_edges)
{
	struct drive_fixture *f = &fixture[0];
	fixture_init(f, 0);
	struct drivebase_command request = {.motion = DRIVEBASE_TIMED,
		.speed = 120, .turn_rate = 60, .duration_ms = 1000,
		.finish = PBIO_CONTROL_ON_COMPLETION_COAST};
	command(f, 0, &request);
	struct drivebase_reference reference;
	struct motor_reference output[2];
	advance(f, 0, &reference, output);
	zassert_not_equal(drivebase_control_init(&f->db, &f->motor[0].control.settings,
		&f->motor[1].control.settings, 1000, 1, 112000, false), 0);
	struct drivebase_settings before, invalid, after;
	drivebase_settings_get(&f->db, &before);
	invalid = before;
	invalid.turn_rate = 0;
	zassert_not_equal(drivebase_settings_set(&f->db, &invalid), 0);
	drivebase_settings_get(&f->db, &after);
	zassert_equal(after.speed, before.speed);
	zassert_equal(after.turn_rate, before.turn_rate);
	advance(f, 50, &reference, output);
	zassert_true(reference.active && (output[0].voltage_mv != 0 ||
		output[1].voltage_mv != 0));

	struct drivebase_command converted = {.motion = DRIVEBASE_TIMED,
		.speed = 123, .finish = PBIO_CONTROL_ON_COMPLETION_COAST};
	zassert_not_equal(drivebase_arc_command(INT32_MAX, INT32_MAX, false,
		false, &converted), 0);
	zassert_not_equal(drivebase_arc_command(1, 90, false, false, &converted), 0);
	zassert_not_equal(drivebase_tank_command(INT32_MIN, 100, 90,
		false, &converted), 0);
	zassert_equal(converted.motion, DRIVEBASE_TIMED);
	zassert_equal(converted.speed, 123);
}

ZTEST_SUITE(drivebase, NULL, NULL, NULL, NULL, NULL);
