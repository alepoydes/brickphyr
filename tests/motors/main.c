// SPDX-License-Identifier: Apache-2.0
#include "hub_ports.h"
#include "hub_clock.h"
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
static uint32_t fail_drive_channel = UINT32_MAX;
static bool fail_first_coast_after_drive_error;
static bool count_b_drive;
static unsigned b_drive_count;
static int capture_pwm(const struct device *dev, uint32_t channel,
	uint32_t period, uint32_t pulse, pwm_flags_t flags)
{
	if (block_output && pulse) {
		block_output = false;
		k_sem_give(&output_entered);
		zassert_ok(k_sem_take(&output_continue, K_MSEC(200)));
	}
	if (pulse && channel == 3 && count_b_drive) b_drive_count++;
	if (pulse && channel == fail_drive_channel) {
		if (fail_first_coast_after_drive_error) {
			fail_first_coast_after_drive_error = false;
			pup_test_circuits[0].coast_error = -EACCES;
		}
		return -EIO;
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

static struct bp_motor_group_state group_settled(uint32_t request, bool active)
{
	int64_t deadline = k_uptime_get() + 150;
	struct bp_motor_group_state state;
	do {
		zassert_ok(hub_motor_group_get(&state));
		if (state.request == request && !state.pending && state.active == active) return state;
		zassert_true(k_uptime_get() < deadline, "Group %u did not settle", request);
		k_msleep(1);
	} while (true);
}

static struct bp_motor_group_command group_command(int32_t a, int32_t b, int64_t deadline)
{
	struct bp_motor_group_command command = {
		.port_mask = BIT(0) | BIT(1), .deadline_ms = deadline,
	};
	command.ports[0].generation = motor(0).generation;
	command.ports[1].generation = motor(1).generation;
	command.ports[0].duty_permille = a;
	command.ports[1].duty_permille = b;
	return command;
}

static void group_lifetime(void)
{
	struct bp_motor_group_command command = group_command(250, -400, k_uptime_get() + 80);
	struct bp_motor_group_state before, state;
	before.request = UINT32_MAX;
	zassert_equal(hub_motor_group_get(&before), -ENODATA);
	zassert_equal(before.request, 0);
	unsigned calls = fake_pwm_set_cycles_fake.call_count;
	uint32_t request = 0;
	command.ports[1].generation++; /* A valid member cannot be claimed first. */
	zassert_equal(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request), -ESTALE);
	state.request = UINT32_MAX;
	zassert_equal(hub_motor_group_get(&state), -ENODATA);
	zassert_equal(state.request, before.request);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, calls);
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);
	zassert_equal(motor(1).owner, BP_PORT_UNOWNED);
	command.ports[1].generation--;
	command.deadline_ms = k_uptime_get();
	zassert_equal(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request), -ETIMEDOUT);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, calls);

	command.deadline_ms = k_uptime_get() + 80;
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request));
	state = group_settled(request, true);
	zassert_ok(state.error);
	zassert_equal(state.port_mask, BIT(0) | BIT(1));
	zassert_equal(state.applied_mask, state.port_mask);
	zassert_equal(state.cleanup_mask, 0);
	zassert_true(state.timestamp_resolution_us > 0);
	zassert_true(state.ports[0].applied_us >= state.requested_ms * 1000);
	zassert_true(state.ports[1].applied_us >= state.ports[0].applied_us);
	zassert_true(state.ports[1].applied_us <= hub_time_us());
	zassert_equal(state.deadline_ms, command.deadline_ms);
	zassert_equal(state.ports[0].request, motor(0).request);
	zassert_equal(state.ports[1].request, motor(1).request);
	zassert_equal(motor(0).duty, 250);
	zassert_equal(motor(1).duty, -400);
	zassert_equal(motor(0).owner, BP_PORT_APPLICATION);
	zassert_equal(motor(1).owner, BP_PORT_APPLICATION);
	zassert_equal(hub_motor_drive(BP_PORT_APPLICATION, 0, 100, 0), -EBUSY);
	struct bp_motor_group_command smaller = command;
	smaller.port_mask = BIT(0);
	uint32_t rejected = 0;
	zassert_equal(hub_motor_group_set(BP_PORT_APPLICATION, &smaller, &rejected), -EBUSY);
	/* Same-mask replacement after completion is allowed; zero duty brakes. */
	command = group_command(0, 350, k_uptime_get() + 60);
	uint32_t previous = request;
	zassert_ok(hub_motor_group_exchange(BP_PORT_APPLICATION, &command, &request, &before));
	zassert_not_equal(request, previous);
	zassert_equal(before.request, previous);
	zassert_false(before.pending);
	zassert_equal(before.applied_mask, BIT(0) | BIT(1));
	for (uint32_t i = 0; i < 2; i++) {
		zassert_equal(before.ports[i].request, state.ports[i].request);
		zassert_equal(before.ports[i].generation, state.ports[i].generation);
		zassert_equal(before.ports[i].applied_us, state.ports[i].applied_us);
		zassert_equal(before.ports[i].duty_permille, i ? -400 : 250);
	}
	zassert_ok(hub_motor_group_get(&state));
	zassert_equal(state.request, request);
	state = group_settled(request, true);
	zassert_equal(state.applied_mask, state.port_mask);
	zassert_equal(motor(0).mode, BP_MOTOR_DUTY);
	zassert_equal(motor(0).duty, 0);
	zassert_equal(pup_test_sensor_supply(0), 3);
	zassert_equal(motor(1).duty, 350);
	before = state;
	int64_t until = command.deadline_ms + 5 - k_uptime_get();
	if (until > 0) k_msleep(until);
	state = group_settled(request, false);
	zassert_equal(state.error, -ETIMEDOUT);
	zassert_equal(state.cleanup_mask, state.port_mask);
	for (uint32_t i = 0; i < 2; i++) {
		zassert_equal(state.ports[i].applied_us, before.ports[i].applied_us);
		zassert_equal(state.ports[i].mode, BP_MOTOR_DUTY);
		zassert_equal(state.ports[i].duty_permille, i ? 350 : 0);
		zassert_true(state.ports[i].cleanup_us >= command.deadline_ms * 1000);
	}
	zassert_equal(motor(0).mode, BP_MOTOR_COAST);
	zassert_equal(motor(1).mode, BP_MOTOR_COAST);
	command = group_command(0, 0, 0);
	command.coast_mask = command.port_mask;
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request));
	state = group_settled(request, false);
	zassert_ok(state.error);
	zassert_equal(state.deadline_ms, 0);
	zassert_equal(state.applied_mask, state.port_mask);
	zassert_equal(state.cleanup_mask, 0);
	zassert_equal(state.ports[0].mode, BP_MOTOR_COAST);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);
	zassert_equal(motor(1).owner, BP_PORT_UNOWNED);

	/* The deadline can pass inside A's driver. B must never receive duty. */
	command = group_command(600, -500, k_uptime_get() + 50);
	b_drive_count = 0;
	count_b_drive = true;
	block_output = true;
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request));
	zassert_ok(k_sem_take(&output_entered, K_MSEC(50)));
	struct bp_motor_group_command replacement = group_command(100, -100, k_uptime_get() + 150);
	zassert_equal(hub_motor_group_exchange(BP_PORT_APPLICATION, &replacement, &rejected, &before), -EBUSY);
	zassert_equal(before.request, request);
	zassert_true(before.pending);
	until = command.deadline_ms + 5 - k_uptime_get();
	if (until > 0) k_msleep(until);
	k_sem_give(&output_continue);
	state = group_settled(request, false);
	count_b_drive = false;
	zassert_equal(state.error, -ETIMEDOUT);
	zassert_equal(b_drive_count, 0);
	zassert_equal(motor(0).mode, BP_MOTOR_COAST);
	zassert_equal(motor(1).mode, BP_MOTOR_COAST);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	/* The deliberately blocked worker also aged passive-device detection. */
	wait_type(0, 1);
	wait_type(1, 2);

	/* B fails after A reaches its driver; A's first coast also fails. */
	command = group_command(200, -300, k_uptime_get() + 150);
	fail_drive_channel = 3; /* Port B reverse PWM channel in this fixture. */
	fail_first_coast_after_drive_error = true;
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request));
	state = group_settled(request, false);
	fail_drive_channel = UINT32_MAX;
	zassert_false(fail_first_coast_after_drive_error);
	zassert_equal(state.error, -EIO);
	zassert_equal(state.cleanup_error, -EACCES);
	zassert_equal(state.ports[0].cleanup_error, -EACCES);
	zassert_equal(state.applied_mask, BIT(0));
	zassert_equal(state.cleanup_mask, BIT(1));
	zassert_true(state.ports[0].applied_us > 0);
	zassert_equal(state.ports[0].cleanup_us, 0);
	zassert_equal(state.ports[1].applied_us, 0);
	zassert_true(state.ports[1].cleanup_us >= state.ports[0].applied_us);
	zassert_equal(state.ports[0].duty_permille, 200);
	zassert_false(motor(0).valid);
	zassert_equal(motor(1).mode, BP_MOTOR_COAST);
	zassert_equal(pup_test_sensor_supply(1), 0);
	zassert_equal(motor(0).owner, BP_PORT_APPLICATION);
	zassert_equal(motor(1).owner, BP_PORT_APPLICATION);
	zassert_equal(pup_test_circuits[0].coast_error, 0);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);
	zassert_equal(motor(1).owner, BP_PORT_UNOWNED);
	zassert_equal(pup_test_sensor_supply(0), 0);
	/* Release repairs the output; the next accepted group returns the failed
	 * group's outcome before replacing its one retained state slot. */
	previous = request;
	command = group_command(0, 0, 0);
	command.coast_mask = command.port_mask;
	zassert_ok(hub_motor_group_exchange(BP_PORT_APPLICATION, &command, &request, &before));
	zassert_equal(before.request, previous);
	zassert_equal(before.error, -EIO);
	zassert_equal(before.cleanup_error, -EACCES);
	zassert_equal(before.ports[0].cleanup_error, -EACCES);
	zassert_equal(before.applied_mask, BIT(0));
	zassert_equal(before.cleanup_mask, BIT(1), "Independent repair must not rewrite settled history");
	state = group_settled(request, false);
	zassert_ok(state.error);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));

	/* A single-member coast cancels the group, but keeps both claims until release. */
	command = group_command(-200, 300, k_uptime_get() + 150);
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request));
	group_settled(request, true);
	zassert_ok(hub_motor_coast(BP_PORT_APPLICATION, 0));
	state = group_settled(request, false);
	zassert_equal(state.error, -ECANCELED);
	zassert_equal(motor(0).mode, BP_MOTOR_COAST);
	zassert_equal(motor(1).mode, BP_MOTOR_COAST);
	zassert_equal(motor(0).owner, BP_PORT_APPLICATION);
	zassert_equal(motor(1).owner, BP_PORT_APPLICATION);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));

	/* Releasing an already-coasting member still cancels its live peer. */
	command = group_command(0, 300, k_uptime_get() + 150);
	command.coast_mask = BIT(0);
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request));
	group_settled(request, true);
	zassert_ok(hub_port_release(BP_PORT_APPLICATION, 0));
	state = group_settled(request, false);
	zassert_equal(state.error, -ECANCELED);
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);
	zassert_equal(motor(1).mode, BP_MOTOR_COAST);
	zassert_equal(motor(1).owner, BP_PORT_APPLICATION);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));

	/* Release cannot free either claim while an original group write is blocked. */
	command = group_command(700, -700, k_uptime_get() + 150);
	block_output = true;
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request));
	zassert_ok(k_sem_take(&output_entered, K_MSEC(50)));
	k_sem_give(&release_started);
	k_msleep(1);
	zassert_equal(motor(0).owner, BP_PORT_APPLICATION);
	zassert_equal(motor(1).owner, BP_PORT_APPLICATION);
	zassert_equal(k_sem_take(&release_finished, K_NO_WAIT), -EBUSY);
	int64_t released_us = hub_time_us();
	k_sem_give(&output_continue);
	zassert_ok(k_sem_take(&release_finished, K_MSEC(50)));
	zassert_ok(release_result);
	state = group_settled(request, false);
	zassert_equal(state.error, -ECANCELED);
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);
	zassert_equal(motor(1).owner, BP_PORT_UNOWNED);
	zassert_equal(state.applied_mask, BIT(0));
	zassert_true(state.ports[0].applied_us >= released_us);
	zassert_equal(state.cleanup_mask, state.port_mask);
	zassert_true(state.ports[0].cleanup_us >= state.ports[0].applied_us);
	zassert_equal(state.ports[1].applied_us, 0);

	/* Cancellation during A's failed write must retain its first coast error,
	 * even though the group's follow-up coast succeeds. */
	command = group_command(300, -300, k_uptime_get() + 150);
	fail_drive_channel = 0;
	fail_first_coast_after_drive_error = true;
	block_output = true;
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request));
	zassert_ok(k_sem_take(&output_entered, K_MSEC(50)));
	zassert_ok(hub_motor_coast(BP_PORT_APPLICATION, 0));
	k_sem_give(&output_continue);
	state = group_settled(request, false);
	fail_drive_channel = UINT32_MAX;
	zassert_false(fail_first_coast_after_drive_error);
	zassert_equal(state.error, -ECANCELED);
	zassert_equal(state.cleanup_error, -EACCES);
	zassert_equal(state.ports[0].cleanup_error, -EACCES);
	zassert_equal(state.applied_mask, 0);
	zassert_equal(state.cleanup_mask, state.port_mask);
	zassert_true(state.ports[0].cleanup_us > 0);
	zassert_equal(pup_test_circuits[0].coast_error, 0);
	zassert_equal(motor(0).mode, BP_MOTOR_COAST);
	zassert_equal(motor(1).mode, BP_MOTOR_COAST);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
}

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
	group_lifetime();

	/* A live controller group excludes even a disjoint group from another owner;
	 * application cleanup cannot coast or release the controller's claim. */
	struct bp_motor_group_command command = group_command(200, 0, k_uptime_get() + 150);
	command.port_mask = BIT(0);
	uint32_t request;
	zassert_ok(hub_motor_group_set(BP_PORT_CONTROLLER, &command, &request));
	group_settled(request, true);
	zassert_equal(hub_motor_drive(BP_PORT_APPLICATION, 0, -200, 0), -EBUSY);
	zassert_equal(hub_port_release(BP_PORT_APPLICATION, 0), -EBUSY);
	command = group_command(0, 200, k_uptime_get() + 150);
	command.port_mask = BIT(1);
	zassert_equal(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request), -EBUSY);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	zassert_equal(motor(0).owner, BP_PORT_CONTROLLER);
	zassert_ok(hub_ports_release(BP_PORT_CONTROLLER));
	zassert_equal(motor(0).owner, BP_PORT_UNOWNED);

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

	command = group_command(1000, -1000, k_uptime_get() + 150);
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &request));
	struct bp_motor_group_state before_shutdown = group_settled(request, true);
	zassert_ok(hub_ports_shutdown());
	struct bp_motor_group_state stopped;
	zassert_ok(hub_motor_group_get(&stopped));
	zassert_equal(stopped.error, -ESHUTDOWN);
	zassert_equal(stopped.cleanup_mask, stopped.port_mask);
	for (uint32_t i = 0; i < 2; i++) {
		zassert_equal(stopped.ports[i].applied_us, before_shutdown.ports[i].applied_us);
		zassert_true(stopped.ports[i].cleanup_us >= stopped.ports[i].applied_us);
	}
	zassert_equal(hub_motor_drive(BP_PORT_APPLICATION, 0, 500, 0), -ESHUTDOWN);
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) zassert_true(pup_test_released(i));
	zassert_equal(hub_motor_activity() & 1, 0);
	calls = fake_pwm_set_cycles_fake.call_count;
	k_msleep(30);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, calls, "PWM outlived shutdown");
}
ZTEST_SUITE(motors, NULL, NULL, NULL, NULL, NULL);
