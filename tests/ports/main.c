// SPDX-License-Identifier: Apache-2.0
#include "hub_ports.h"
#include "wiring.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

static K_SEM_DEFINE(paused, 0, 1);
static K_SEM_DEFINE(resume, 0, 1);
static void pause_workqueue(struct k_work *item)
{
	k_sem_give(&paused);
	zassert_ok(k_sem_take(&resume, K_MSEC(150)));
}
static K_WORK_DEFINE(blocker, pause_workqueue);

static struct bp_port_state snapshot(uint32_t port)
{
	struct bp_port_state value;
	zassert_ok(hub_port_get(port, &value));
	return value;
}

static struct bp_port_state wait_type(uint32_t port, uint16_t id)
{
	/* A full scan takes at most ten settling intervals. Twenty consecutive
	 * confirmations plus initial discovery fit in a second at this tick rate. */
	int64_t deadline = k_uptime_get() + 1000;
	struct bp_port_state value;
	do {
		value = snapshot(port);
		zassert_true(k_uptime_get() < deadline, "Port %u: phase %u, error %d, ID %u",
			port, value.phase, value.error, value.type_id);
		if (!value.error && value.type_id == id) {
			return value;
		}
		k_msleep(1);
	} while (true);
}

ZTEST(ports, test_detection)
{
	zassert_true(pup_test_supply());
	zassert_equal(snapshot(0).error, -EAGAIN);
	zassert_equal(hub_port_get(6, &(struct bp_port_state){0}), -EINVAL);
	zassert_equal(hub_port_get(0, NULL), -EINVAL);
	/* Six independent circuits are detected concurrently. No test supplies
	 * a result by calling the production debounce/classification functions. */
	pup_test_circuits[0].kind = MEDIUM_MOTOR;
	pup_test_circuits[1].kind = TRAIN_MOTOR;
	pup_test_circuits[2].kind = LIGHT;
	pup_test_circuits[3].kind = UART_DEVICE;
	pup_test_circuits[4].kind = TOUCH_SWITCH;
	const uint16_t ids[] = {1, 2, 8, 14, 5, 0};
	for (uint32_t i = 0; i < 6; i++) {
		struct bp_port_state value = wait_type(i, ids[i]);
		zassert_equal(value.failures, 0, "GPIO settling was too short");
		zassert_equal(value.phase, i == 3 ? BP_PORT_UART : i == 5 ? BP_PORT_EMPTY : BP_PORT_PASSIVE);
		zassert_equal(value.pressed, 0);
	}
	const struct shell *shell = shell_backend_dummy_get_ptr();
	uint32_t reads = pup_test_circuits[0].reads;
	zassert_ok(shell_execute_cmd(shell, "observe port.A"));
	size_t length;
	const char *output = shell_backend_dummy_get_output(shell, &length);
	zassert_not_null(strstr(output, "type_id=1"));
	zassert_equal(pup_test_circuits[0].reads, reads, "Observation started GPIO I/O");

	uint32_t generation = snapshot(4).generation;
	pup_test_circuits[4].pressed = true;
	k_msleep(20);
	zassert_equal(snapshot(4).pressed, 1);
	zassert_equal(snapshot(4).generation, generation, "Press is not a reconnect");

	/* First conflicting completed scan invalidates the old identity; it must
	 * not remain usable through the entire new-device debounce interval. */
	generation = snapshot(0).generation;
	pup_test_circuits[0].kind = UNPLUGGED;
	k_msleep(60);
	struct bp_port_state value = snapshot(0);
	zassert_equal(value.phase, BP_PORT_DETECTING);
	zassert_equal(value.error, -EAGAIN);
	zassert_equal(value.type_id, 0);
	zassert_true(value.generation > generation);
	value = wait_type(0, 0);
	zassert_equal(value.phase, BP_PORT_EMPTY);
	generation = value.generation;
	pup_test_circuits[0].kind = UART_DEVICE;
	value = wait_type(0, 14);
	zassert_true(value.generation > generation);

	/* A read failure wins over a later release failure. Recovery restarts
	 * classification, and errors on A do not suspend other ports. */
	pup_test_circuits[0].read_error = -EIO;
	pup_test_circuits[0].release_error = -EBUSY;
	k_msleep(20);
	value = snapshot(0);
	zassert_equal(value.error, -EIO);
	zassert_equal(value.cleanup_error, -EBUSY);
	zassert_equal(value.phase, BP_PORT_FAULT);
	zassert_equal(value.type_id, 0);
	zassert_true(value.failures > 0);
	uint32_t sequence = snapshot(1).sequence;
	k_msleep(40);
	zassert_true(snapshot(1).sequence > sequence);
	zassert_ok(snapshot(1).error);
	pup_test_circuits[0].release_error = 0;
	k_msleep(110);
	zassert_equal(snapshot(0).error, -EIO);
	zassert_ok(snapshot(0).cleanup_error);
	zassert_true(pup_test_released(0));
	pup_test_circuits[0].read_error = 0;
	value = wait_type(0, 14);
	zassert_true(value.failures > 0);
	zassert_ok(value.cleanup_error);

	/* Cache reads cannot hide starvation of the shared service queue. */
	k_work_submit(&blocker);
	zassert_ok(k_sem_take(&paused, K_MSEC(20)));
	value = snapshot(0);
	reads = pup_test_circuits[0].reads;
	k_msleep(60);
	zassert_equal(snapshot(0).error, -ETIMEDOUT);
	zassert_equal(snapshot(0).sample_ms, value.sample_ms);
	zassert_equal(pup_test_circuits[0].reads, reads);
	k_sem_give(&resume);
	value = wait_type(0, 14);

	zassert_ok(hub_ports_shutdown());
	zassert_false(pup_test_supply());
	reads = 0;
	for (uint32_t i = 0; i < 6; i++) {
		zassert_true(pup_test_released(i));
		zassert_equal(snapshot(i).phase, BP_PORT_OFF);
		zassert_equal(snapshot(i).error, -ESHUTDOWN);
		zassert_equal(snapshot(i).type_id, 0);
		reads += pup_test_circuits[i].reads;
	}
	k_msleep(50);
	uint32_t after = 0;
	for (uint32_t i = 0; i < 6; i++) {
		after += pup_test_circuits[i].reads;
	}
	zassert_equal(reads, after, "GPIO access survived shutdown");
}
ZTEST_SUITE(ports, NULL, NULL, NULL, NULL, NULL);
