// SPDX-License-Identifier: Apache-2.0
#include "control.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
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

ZTEST_SUITE(motor_control, NULL, NULL, NULL, NULL, NULL);
