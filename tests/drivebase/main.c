// SPDX-License-Identifier: Apache-2.0
/* Two synthetic UART encoders and a simple PWM-driven shaft plant exercise the
 * real paired port worker. This does not establish trajectory accuracy. */
#include "hub_ports.h"
#include "hub_battery.h"
#include "wiring.h"
#include "../lump/captures.h"
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/pwm/pwm_fake.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;
static const struct device *const uart[] = {
	DEVICE_DT_GET(DT_NODELABEL(left_uart)), DEVICE_DT_GET(DT_NODELABEL(right_uart))};
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
static atomic_t pwm_channels[4];
static atomic_t reporting[2];
static atomic_t fail_right_pwm;
static int32_t shaft_angle[2], shaft_step[2];

static void sent(const struct device *dev, size_t size, void *arg)
{
	uint8_t bytes[128];
	zassert_equal(uart_emul_get_tx_data(dev, bytes, sizeof(bytes)), size);
}

static int pwm(const struct device *dev, uint32_t channel,
	uint32_t period, uint32_t pulse, pwm_flags_t flags)
{
	if (channel >= 2 && channel < 4 && pulse && atomic_get(&fail_right_pwm)) {
		atomic_set(&fail_right_pwm, 0);
		return -EIO;
	}
	if (channel < ARRAY_SIZE(pwm_channels)) atomic_set(&pwm_channels[channel], pulse);
	return 0;
}

static void angle_frame(struct k_timer *timer)
{
	for (size_t port = 0; port < 2; port++) {
		if (!atomic_get(&reporting[port])) continue;
		/* Synthetic first-order inertia; no controller formula is duplicated. */
		int32_t target = (atomic_get(&pwm_channels[port * 2]) -
			atomic_get(&pwm_channels[port * 2 + 1])) / 20;
		shaft_step[port] = (3 * shaft_step[port] + target) / 4;
		shaft_angle[port] = (shaft_angle[port] + shaft_step[port]) % 3600;
		if (shaft_angle[port] < 0) shaft_angle[port] += 3600;
		uint8_t data[] = {0xd4, 0, 0, 0, 0, 0x2b};
		data[3] = shaft_angle[port];
		data[4] = shaft_angle[port] >> 8;
		data[5] ^= data[3] ^ data[4];
		zassert_equal(uart_emul_put_rx_data(uart[port], data, sizeof(data)), sizeof(data));
	}
}
K_TIMER_DEFINE(encoder, angle_frame, NULL);

static int voltage(const struct device *dev, unsigned int channel, void *context, uint32_t *out)
{
	*out = 3100;
	return 0;
}

static void feed(uint32_t port, const uint8_t *data, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		zassert_equal(uart_emul_put_rx_data(uart[port], data + i, 1), 1);
		k_msleep(5);
	}
}

static void feed_both(const uint8_t *data, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		for (size_t port = 0; port < 2; port++) {
			zassert_equal(uart_emul_put_rx_data(uart[port], data + i, 1), 1);
		}
		k_msleep(5);
	}
}

static struct bp_motor_state motor(uint32_t port)
{
	struct bp_motor_state value;
	zassert_ok(hub_motor_get(port, &value));
	return value;
}

static size_t allocated_bytes(void)
{
	struct k_heap *heaps;
	int count = k_heap_array_get(&heaps);
	size_t total = 0;
	for (int i = 0; i < count; i++) {
		struct sys_memory_stats stats;
		k_spinlock_key_t key = k_spin_lock(&heaps[i].lock);
		int err = sys_heap_runtime_stats_get(&heaps[i].heap, &stats);
		k_spin_unlock(&heaps[i].lock, key);
		zassert_ok(err);
		total += stats.allocated_bytes;
	}
	return total;
}

static struct bp_motor_state motor_applied(uint32_t port, uint32_t request)
{
	int64_t deadline = k_uptime_get() + 300;
	for (;;) {
		struct bp_motor_state value = motor(port);
		if (!value.pending && value.completed == request) return value;
		zassert_true(k_uptime_get() < deadline, "Motor %u request %u stalled", port, request);
		k_msleep(1);
	}
}

static struct bp_drivebase_state pair_applied(uint32_t handle, uint32_t request)
{
	int64_t deadline = k_uptime_get() + 300;
	for (;;) {
		struct bp_drivebase_state value;
		zassert_ok(hub_drivebase_get(BP_PORT_APPLICATION, handle, &value));
		if (!value.pending && value.completed == request) {
			zassert_ok(value.error);
			zassert_ok(value.command_error);
			zassert_ok(value.cleanup_error);
			return value;
		}
		zassert_true(k_uptime_get() < deadline, "Pair request %u stalled", request);
		k_msleep(1);
	}
}

static void wait_phase(uint32_t port, uint32_t phase)
{
	int64_t deadline = k_uptime_get() + 1000;
	for (;;) {
		struct bp_port_state value;
		zassert_ok(hub_port_get(port, &value));
		if (value.phase == phase) return;
		zassert_true(k_uptime_get() < deadline, "Port %u phase %u error %d",
			port, value.phase, value.error);
		k_msleep(1);
	}
}

static void wait_sensor(uint32_t port)
{
	int64_t deadline = k_uptime_get() + 1000;
	for (;;) {
		struct bp_sensor_sample sample;
		zassert_ok(hub_sensor_get(port, &sample));
		if (!sample.error) return;
		zassert_true(k_uptime_get() < deadline, "Port %u sensor error %d", port, sample.error);
		k_msleep(1);
	}
}

static uint32_t generation(uint32_t port)
{
	struct bp_port_state value;
	zassert_ok(hub_port_get(port, &value));
	return value.generation;
}

static void configure_motor(uint32_t port, int32_t gear_ratio)
{
	struct bp_servo_setup setup = {.direction = 1, .gear_ratio = gear_ratio};
	uint32_t request;
	zassert_ok(hub_servo_setup(BP_PORT_APPLICATION, port, generation(port), &setup, &request));
	struct bp_motor_state value = motor_applied(port, request);
	zassert_ok(value.error);
	int64_t deadline = k_uptime_get() + 300;
	for (;;) {
		struct bp_servo_state servo;
		int err = hub_servo_get(port, generation(port), 100, &servo);
		if (!err && !servo.pending) return;
		zassert_true(k_uptime_get() < deadline, "Motor %u observer not ready: %d", port, err);
		k_msleep(1);
	}
}

static struct bp_drivebase_setup pair_setup(void)
{
	return (struct bp_drivebase_setup){
		.ports = {0, 1}, .generations = {generation(0), generation(1)},
		.wheel_um = 56000, .track_um = 112000};
}

static uint32_t open_pair(void)
{
	struct bp_drivebase_setup setup = pair_setup();
	uint32_t handle;
	zassert_ok(hub_drivebase_open(BP_PORT_APPLICATION, &setup, &handle));
	struct bp_drivebase_state value = pair_applied(handle, 1);
	zassert_true(value.valid);
	return handle;
}

static struct bp_drivebase_state command_pair(uint32_t handle, struct bp_drivebase_command command)
{
	uint32_t request;
	zassert_ok(hub_drivebase_command(BP_PORT_APPLICATION, handle, &command, &request));
	return pair_applied(handle, request);
}

static void wait_pair_gone(uint32_t handle)
{
	int64_t deadline = k_uptime_get() + 300;
	for (;;) {
		struct bp_drivebase_state value;
		if (hub_drivebase_get(BP_PORT_APPLICATION, handle, &value) == -ENODEV) return;
		zassert_true(k_uptime_get() < deadline, "Pair association survived motor change");
		k_msleep(1);
	}
}

static void wait_coast(uint32_t port)
{
	int64_t deadline = k_uptime_get() + 300;
	for (;;) {
		struct bp_motor_state value = motor(port);
		if (!value.pending && value.mode == BP_MOTOR_COAST && !value.control.active) return;
		zassert_true(k_uptime_get() < deadline, "Motor %u did not coast", port);
		k_msleep(1);
	}
}

static void wait_released(uint32_t port)
{
	int64_t deadline = k_uptime_get() + 300;
	for (;;) {
		struct bp_motor_state value = motor(port);
		if (!value.pending && value.owner == BP_PORT_UNOWNED && value.mode == BP_MOTOR_COAST) return;
		zassert_true(k_uptime_get() < deadline, "Motor %u was not released", port);
		k_msleep(1);
	}
}

static void wait_output(uint32_t port)
{
	int64_t deadline = k_uptime_get() + 300;
	while (!atomic_get(&pwm_channels[port * 2]) && !atomic_get(&pwm_channels[port * 2 + 1])) {
		zassert_true(k_uptime_get() < deadline, "Motor %u had no output", port);
		k_msleep(1);
	}
}

static void connect_motors(void)
{
	fake_pwm_set_cycles_fake.custom_fake = pwm;
	zassert_ok(adc_emul_const_raw_value_set(adc, 10, 0));
	zassert_ok(adc_emul_raw_value_func_set(adc, 11, voltage, NULL));
	for (size_t port = 0; port < 2; port++) {
		uart_emul_callback_tx_data_ready_set(uart[port], sent, NULL);
		pup_test_circuits[port].kind = UART_DEVICE;
	}
	for (size_t port = 0; port < 2; port++) wait_phase(port, BP_PORT_NEGOTIATING);
	uint8_t ack = 4;
	feed_both(&ack, 1);
	feed_both(technic_large_motor, sizeof(technic_large_motor));
	for (size_t port = 0; port < 2; port++) atomic_set(&reporting[port], 1);
	k_timer_start(&encoder, K_MSEC(5), K_MSEC(5));
	for (size_t port = 0; port < 2; port++) wait_sensor(port);
	struct bp_battery_sample battery;
	zassert_ok(hub_battery_get(&battery));
	zassert_ok(battery.error);
	configure_motor(0, 1000);
	configure_motor(1, 1000);
}

ZTEST(drivebase, test_pair_lifetime)
{
	connect_motors();
	size_t heap_before_pair = allocated_bytes();
	uint32_t handle = open_pair();
	zassert_true(allocated_bytes() > heap_before_pair);
	struct bp_drivebase_state initial;
	zassert_ok(hub_drivebase_get(BP_PORT_APPLICATION, handle, &initial));
	struct bp_drivebase_settings settings = initial.settings;
	zassert_true(settings.speed > 1);
	settings.speed /= 2;
	uint32_t settings_request;
	zassert_ok(hub_drivebase_settings_set(BP_PORT_APPLICATION, handle, &settings, &settings_request));
	zassert_equal(pair_applied(handle, settings_request).settings.speed, settings.speed);
	struct bp_drivebase_state value = command_pair(handle,
		(struct bp_drivebase_command){.action = BP_DRIVEBASE_RELATIVE, .distance = 1000,
			.finish = BP_MOTOR_FINISH_HOLD});
	zassert_true(value.active);
	wait_output(0);
	wait_output(1);
	value = command_pair(handle, (struct bp_drivebase_command){.action = BP_DRIVEBASE_COAST});
	zassert_false(value.active);
	wait_coast(0);
	wait_coast(1);

	value = command_pair(handle, (struct bp_drivebase_command){.action = BP_DRIVEBASE_TIMED,
		.speed = 150, .turn_rate = 0, .duration_ms = 300000,
		.finish = BP_MOTOR_FINISH_CONTINUE});
	zassert_true(value.active);
	uint32_t motion = value.motion;
	zassert_ok(hub_drivebase_settings_set(BP_PORT_APPLICATION, handle, &settings, &settings_request));
	value = pair_applied(handle, settings_request);
	zassert_equal(value.motion, motion);
	zassert_true(value.active);
	uint32_t request;
	struct bp_servo_command single = {.action = BP_SERVO_RUN, .speed = 150};
	zassert_ok(hub_servo_move(BP_PORT_APPLICATION, 0, generation(0), &single, &request));
	zassert_ok(motor_applied(0, request).error);
	wait_coast(1);
	zassert_ok(hub_drivebase_get(BP_PORT_APPLICATION, handle, &value));
	zassert_false(value.pending || value.active);
	zassert_true(value.done);
	zassert_equal(value.command_error, -ECANCELED);
	/* An individual motor command stops the pair, but the handle remains
	 * associated and can be used for the next paired command. */
	value = command_pair(handle, (struct bp_drivebase_command){.action = BP_DRIVEBASE_TIMED,
		.speed = 150, .duration_ms = 300000, .finish = BP_MOTOR_FINISH_CONTINUE});
	zassert_true(value.active);
	value = command_pair(handle, (struct bp_drivebase_command){.action = BP_DRIVEBASE_COAST});
	zassert_false(value.active);
	wait_coast(0);
	wait_coast(1);

	zassert_ok(hub_drivebase_close(BP_PORT_APPLICATION, handle));
	wait_pair_gone(handle);
	wait_coast(0);
	wait_coast(1);
	zassert_equal(allocated_bytes(), heap_before_pair);

	handle = open_pair();
	zassert_ok(hub_motor_close(BP_PORT_APPLICATION, 1, generation(1)));
	wait_pair_gone(handle);
	wait_coast(0);
	wait_released(1);
	wait_sensor(1);
	configure_motor(1, 1000);
	handle = open_pair();
	zassert_ok(hub_drivebase_close(BP_PORT_APPLICATION, handle));
	wait_pair_gone(handle);
	wait_coast(0);
	wait_coast(1);

	/* A lost peer invalidates the old association and leaves the survivor
	 * available; reconnect creates a new generation and can form a new pair. */
	handle = open_pair();
	uint32_t old_generation = generation(0);
	atomic_set(&reporting[0], 0);
	pup_test_circuits[0].kind = UNPLUGGED;
	wait_phase(0, BP_PORT_EMPTY);
	wait_pair_gone(handle);
	wait_coast(1);
	zassert_true(generation(0) > old_generation);
	pup_test_circuits[0].kind = UART_DEVICE;
	wait_phase(0, BP_PORT_NEGOTIATING);
	uint8_t ack = 4;
	feed(0, &ack, 1);
	feed(0, technic_large_motor, sizeof(technic_large_motor));
	atomic_set(&reporting[0], 1);
	wait_sensor(0);
	configure_motor(0, 1000);
	handle = open_pair();
	zassert_ok(hub_drivebase_close(BP_PORT_APPLICATION, handle));
	wait_pair_gone(handle);
	wait_coast(0);
	wait_coast(1);

	/* Rejected pair setups may not alter independently commanded outputs. */
	uint32_t left_request, right_request;
	zassert_ok(hub_motor_dc(BP_PORT_APPLICATION, 0, generation(0), 300, 0, &left_request));
	zassert_ok(hub_motor_dc(BP_PORT_APPLICATION, 1, generation(1), -300, 0, &right_request));
	zassert_ok(motor_applied(0, left_request).error);
	zassert_ok(motor_applied(1, right_request).error);
	int32_t left_duty = motor(0).duty, right_duty = motor(1).duty;
	int32_t output_before[4];
	for (size_t i = 0; i < ARRAY_SIZE(output_before); i++) {
		output_before[i] = atomic_get(&pwm_channels[i]);
	}
	struct bp_drivebase_setup setup = pair_setup();
	setup.ports[1] = setup.ports[0];
	setup.generations[1] = setup.generations[0];
	uint32_t rejected;
	zassert_equal(hub_drivebase_open(BP_PORT_APPLICATION, &setup, &rejected), -EINVAL);
	zassert_equal(motor(0).request, left_request);
	zassert_equal(motor(1).request, right_request);
	zassert_equal(motor(0).duty, left_duty);
	zassert_equal(motor(1).duty, right_duty);
	for (size_t i = 0; i < ARRAY_SIZE(output_before); i++) {
		zassert_equal(atomic_get(&pwm_channels[i]), output_before[i]);
	}

	/* A valid setup must fail before touching either running motor when the
	 * shared heap has no space for the pair record. Link the small filler
	 * blocks in place so exhaustion needs no separate allocation. */
	struct heap_filler { struct heap_filler *next; };
	struct heap_filler *fillers = NULL, *block;
	while ((block = k_malloc(64)) != NULL) {
		block->next = fillers;
		fillers = block;
	}
	zassert_not_null(fillers);
	setup = pair_setup();
	zassert_equal(hub_drivebase_open(BP_PORT_APPLICATION, &setup, &rejected), -ENOMEM);
	zassert_equal(motor(0).request, left_request);
	zassert_equal(motor(1).request, right_request);
	zassert_equal(motor(0).duty, left_duty);
	zassert_equal(motor(1).duty, right_duty);
	for (size_t i = 0; i < ARRAY_SIZE(output_before); i++) {
		zassert_equal(atomic_get(&pwm_channels[i]), output_before[i]);
	}
	while (fillers) {
		block = fillers;
		fillers = block->next;
		k_free(block);
	}

	zassert_ok(hub_motor_close(BP_PORT_APPLICATION, 1, generation(1)));
	wait_released(1);
	wait_sensor(1);
	configure_motor(1, 2000);
	zassert_ok(hub_motor_dc(BP_PORT_APPLICATION, 1, generation(1), -300, 0, &right_request));
	zassert_ok(motor_applied(1, right_request).error);
	right_duty = motor(1).duty;
	for (size_t i = 0; i < ARRAY_SIZE(output_before); i++) {
		output_before[i] = atomic_get(&pwm_channels[i]);
	}
	setup = pair_setup();
	zassert_equal(hub_drivebase_open(BP_PORT_APPLICATION, &setup, &rejected), -EINVAL);
	zassert_equal(motor(0).request, left_request);
	zassert_equal(motor(1).request, right_request);
	zassert_equal(motor(0).duty, left_duty);
	zassert_equal(motor(1).duty, right_duty);
	for (size_t i = 0; i < ARRAY_SIZE(output_before); i++) {
		zassert_equal(atomic_get(&pwm_channels[i]), output_before[i]);
	}

	/* A failed second wheel output must stop both; the reported cause remains
	 * the driver error even though the cleanup PWM writes succeed. */
	zassert_ok(hub_motor_close(BP_PORT_APPLICATION, 1, generation(1)));
	wait_released(1);
	wait_sensor(1);
	configure_motor(1, 1000);
	handle = open_pair();
	atomic_set(&fail_right_pwm, 1);
	zassert_ok(hub_drivebase_command(BP_PORT_APPLICATION, handle,
		&(struct bp_drivebase_command){.action = BP_DRIVEBASE_TIMED, .speed = 150,
			.duration_ms = 300000, .finish = BP_MOTOR_FINISH_CONTINUE}, &request));
	int64_t deadline = k_uptime_get() + 300;
	for (;;) {
		zassert_ok(hub_drivebase_get(BP_PORT_APPLICATION, handle, &value));
		if (value.error == -EIO) break;
		zassert_true(k_uptime_get() < deadline, "Pair missed second-wheel output failure");
		k_msleep(1);
	}
	atomic_set(&fail_right_pwm, 0);
	zassert_false(value.pending || value.active);
	zassert_equal(value.command_error, -EIO);
	zassert_ok(value.cleanup_error);
	wait_coast(0);
	wait_coast(1);
	zassert_ok(hub_drivebase_close(BP_PORT_APPLICATION, handle));
	wait_pair_gone(handle);
	for (size_t port = 0; port < 2; port++) {
		zassert_ok(hub_motor_close(BP_PORT_APPLICATION, port, generation(port)));
		wait_released(port);
		wait_sensor(port);
		configure_motor(port, 1000);
	}
	handle = open_pair();
	zassert_true(command_pair(handle, (struct bp_drivebase_command){.action = BP_DRIVEBASE_TIMED,
		.speed = 150, .duration_ms = 300000,
		.finish = BP_MOTOR_FINISH_CONTINUE}).active);
	command_pair(handle, (struct bp_drivebase_command){.action = BP_DRIVEBASE_COAST});
	zassert_ok(hub_drivebase_close(BP_PORT_APPLICATION, handle));
	wait_pair_gone(handle);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	k_timer_stop(&encoder);
}
ZTEST_SUITE(drivebase, NULL, NULL, NULL, NULL, NULL);
