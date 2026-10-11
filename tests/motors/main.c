// SPDX-License-Identifier: Apache-2.0
#include "hub_ports.h"
#include "wiring.h"
#include <string.h>
#include <zephyr/drivers/pwm/pwm_fake.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;

static struct bp_motor_state motor(uint32_t port)
{
	struct bp_motor_state value;
	zassert_ok(hub_motor_get(port, &value));
	return value;
}

static void wait_type(uint32_t port, uint16_t id)
{
	int64_t deadline = k_uptime_get() + 1000;
	struct bp_port_state value;
	do {
		zassert_ok(hub_port_get(port, &value));
		if (!value.error && value.type_id == id) return;
		zassert_true(k_uptime_get() < deadline, "port %u error %d ID %u", port, value.error, value.type_id);
		k_msleep(1);
	} while (true);
}

static struct bp_motor_state completed(uint32_t port)
{
	int64_t deadline = k_uptime_get() + 100;
	struct bp_motor_state value;
	do {
		value = motor(port);
		if (!value.pending) return value;
		zassert_true(k_uptime_get() < deadline, "Port %u command did not complete", port);
		k_msleep(1);
	} while (true);
}

static void drive(enum bp_port_owner owner, uint32_t port, int32_t duty, uint32_t duration)
{
	zassert_ok(hub_motor_drive(owner, port, duty, duration));
	struct bp_motor_state value = completed(port);
	zassert_ok(value.error);
	zassert_true(value.valid);
	zassert_equal(value.mode, BP_MOTOR_DUTY);
	zassert_equal(value.duty, duty);
	zassert_equal(value.owner, owner);
}

static K_SEM_DEFINE(output_entered, 0, 1);
static K_SEM_DEFINE(output_continue, 0, 1);
static bool block_output;
static int output_error;
static int capture_pwm(const struct device *dev, uint32_t channel,
	uint32_t period, uint32_t pulse, pwm_flags_t flags)
{
	if (block_output && pulse) {
		block_output = false;
		k_sem_give(&output_entered);
		zassert_ok(k_sem_take(&output_continue, K_MSEC(200)));
	}
	return output_error;
}

static K_SEM_DEFINE(release_started, 0, 1);
static K_SEM_DEFINE(release_finished, 0, 1);
static int release_result;
static void release_application(void *a, void *b, void *c)
{
	for (;;) {
		k_sem_take(&release_started, K_FOREVER);
		release_result = hub_ports_release(BP_PORT_APPLICATION);
		k_sem_give(&release_finished);
	}
}
K_THREAD_DEFINE(releaser, 2048, release_application, NULL, NULL, NULL, 5, 0, 0);

ZTEST(motors, test_lifetime)
{
	fake_pwm_set_cycles_fake.custom_fake = capture_pwm;
	pup_test_circuits[0].kind = MEDIUM_MOTOR;
	pup_test_circuits[1].kind = TRAIN_MOTOR;
	pup_test_circuits[2].kind = LIGHT;
	wait_type(0, 1);
	wait_type(1, 2);
	wait_type(2, 8);
	wait_type(5, 0);
	unsigned calls = fake_pwm_set_cycles_fake.call_count;
	zassert_equal(hub_motor_drive(BP_PORT_APPLICATION, 0, 1001, 0), -EINVAL);
	zassert_equal(hub_motor_drive(BP_PORT_APPLICATION, 0, 1, UINT32_MAX), -EINVAL);
	zassert_equal(hub_motor_drive(BP_PORT_UNOWNED, 0, 1, 0), -EINVAL);
	zassert_equal(hub_motor_drive(BP_PORT_APPLICATION, 2, 500, 0), -ENOTSUP);
	zassert_equal(hub_motor_drive(BP_PORT_APPLICATION, 5, 500, 0), -ENODEV);
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, calls, "Rejected commands touched PWM");

	uint32_t token = hub_motor_activity();
	drive(BP_PORT_APPLICATION, 0, 250, 0);
	zassert_true(hub_motor_activity() & 1);
	zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 250);
	zassert_equal(pup_test_sensor_supply(0), 2);
	zassert_equal(hub_motor_drive(BP_PORT_TERMINAL, 0, -500, 0), -EBUSY);
	zassert_equal(hub_port_release(BP_PORT_TERMINAL, 0), -EBUSY);
	k_msleep(100); /* Several passive scans must preserve the claim and output. */
	zassert_equal(motor(0).owner, BP_PORT_APPLICATION);
	zassert_equal(pup_test_sensor_supply(0), 2);

	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(shell_execute_cmd(shell, "motor B duty -600 20"));
	struct bp_motor_state value = completed(1);
	zassert_equal(value.duty, -600);
	zassert_equal(value.deadline_ms - value.applied_ms, 20);
	k_msleep(25);
	value = motor(1);
	zassert_equal(value.mode, BP_MOTOR_COAST);
	zassert_equal(value.owner, BP_PORT_TERMINAL, "Expiry must not transfer ownership");
	zassert_equal(pup_test_sensor_supply(1), 0);
	zassert_ok(shell_execute_cmd(shell, "motor B brake"));
	value = completed(1);
	zassert_equal(value.mode, BP_MOTOR_DUTY);
	zassert_equal(value.duty, 0);
	zassert_equal(pup_test_sensor_supply(1), 3);
	zassert_ok(shell_execute_cmd(shell, "observe motor.B"));
	size_t length;
	const char *output = shell_backend_dummy_get_output(shell, &length);
	zassert_not_null(strstr(output, "duty_permille=0"));
	zassert_equal(shell_execute_cmd(shell, "motor B duty 500 -1"), -EINVAL);
	zassert_equal(shell_execute_cmd(shell, "motor B brake extra extra"), -EINVAL);
	hub_ports_session_closed(BP_PORT_TERMINAL);
	value = completed(1);
	zassert_equal(value.owner, BP_PORT_UNOWNED);
	zassert_equal(pup_test_sensor_supply(1), 0);
	zassert_equal(motor(0).owner, BP_PORT_APPLICATION);
	zassert_equal(pup_test_sensor_supply(0), 2, "Host disconnect stopped an autonomous motor");
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);
	zassert_equal(hub_motor_activity() & 1, 0);
	zassert_not_equal(hub_motor_activity(), token, "Start/stop must invalidate an IMU learning window");

	/* Block the driver while a supervisor releases an aborted application's
	 * pending command. Ownership must persist through the final coast. */
	block_output = true;
	zassert_ok(hub_motor_drive(BP_PORT_APPLICATION, 0, 700, 0));
	zassert_ok(k_sem_take(&output_entered, K_MSEC(50)));
	k_sem_give(&release_started);
	k_msleep(1);
	zassert_true(motor(0).releasing);
	zassert_equal(hub_motor_drive(BP_PORT_TERMINAL, 0, 400, 0), -EBUSY);
	zassert_equal(k_sem_take(&release_finished, K_NO_WAIT), -EBUSY);
	k_sem_give(&output_continue);
	zassert_ok(k_sem_take(&release_finished, K_MSEC(50)));
	zassert_ok(release_result);
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);
	zassert_equal(pup_test_sensor_supply(0), 0);

	/* Original command and cleanup failures remain distinct; a failed release
	 * retains the owner until an explicit successful cleanup retry. */
	drive(BP_PORT_APPLICATION, 0, 250, 20);
	output_error = -EIO;
	pup_test_circuits[0].coast_error = -EACCES;
	zassert_ok(hub_motor_drive(BP_PORT_APPLICATION, 0, 300, 20));
	value = completed(0);
	zassert_equal(value.error, -EIO);
	zassert_equal(value.cleanup_error, -EACCES);
	zassert_false(value.valid);
	zassert_equal(value.deadline_ms, 0);
	zassert_true(hub_motor_activity() & 1);
	zassert_equal(hub_ports_release(BP_PORT_APPLICATION), -EIO);
	zassert_equal(motor(0).owner, BP_PORT_APPLICATION);
	zassert_true(motor(0).releasing);
	zassert_equal(hub_motor_drive(BP_PORT_TERMINAL, 0, 10, 0), -EBUSY);
	output_error = 0;
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);
	zassert_equal(motor(0).error, -EIO);
	zassert_equal(hub_motor_activity() & 1, 0);

	drive(BP_PORT_APPLICATION, 0, -900, 0);
	uint32_t generation = motor(0).generation;
	pup_test_circuits[0].kind = UNPLUGGED;
	k_msleep(60);
	zassert_equal(pup_test_sensor_supply(0), 0);
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);
	zassert_true(motor(0).generation > generation);
	wait_type(0, 0);
	pup_test_circuits[0].kind = MEDIUM_MOTOR;
	wait_type(0, 1);
	zassert_equal(motor(0).mode, BP_MOTOR_COAST, "Reconnect restarted the old command");

	drive(BP_PORT_APPLICATION, 0, 1000, 0);
	drive(BP_PORT_TERMINAL, 1, -1000, 0);
	zassert_ok(hub_ports_shutdown());
	zassert_equal(hub_motor_drive(BP_PORT_APPLICATION, 0, 500, 0), -ESHUTDOWN);
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) zassert_true(pup_test_released(i));
	zassert_equal(hub_motor_activity() & 1, 0);
	calls = fake_pwm_set_cycles_fake.call_count;
	k_msleep(30);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, calls, "PWM outlived shutdown");
}
ZTEST_SUITE(motors, NULL, NULL, NULL, NULL, NULL);
