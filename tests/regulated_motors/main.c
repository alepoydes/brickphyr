// SPDX-License-Identifier: Apache-2.0
/* Synthetic UART encoder and ADC, real port owner/controller/output path.
 * The shaft stays fixed: this checks commands, stall and cleanup, not tuning. */
#include "hub_ports.h"
#include "hub_battery.h"
#include "control.h"
#include "wiring.h"
#include "../lump/captures.h"
#include <lego/device.h>
#include <string.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/pwm/pwm_fake.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/atomic.h>

DEFINE_FFF_GLOBALS;
static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(lump_uart));
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
static bool report_angle;
static int16_t shaft_angle, shaft_increment;
static int battery_error;
static uint32_t battery_raw = 3100;
static uint32_t pwm_delay;
static bool block_pwm;
static atomic_t block_observer;
static atomic_t fail_observer;
static K_SEM_DEFINE(worker_entered, 0, 1);
static K_SEM_DEFINE(worker_continue, 0, 1);

int __real_motor_control_observe(struct motor_control *motor, uint32_t ticks,
	int64_t measured, bool coast, struct motor_observation *out);

int __wrap_motor_control_observe(struct motor_control *motor, uint32_t ticks,
	int64_t measured, bool coast, struct motor_observation *out)
{
	/* Keep the real computation, but hold one passive frame before it reads
	 * the controller so release can race the worker outside the port lock. */
	if (coast && atomic_cas(&block_observer, 1, 0)) {
		k_sem_give(&worker_entered);
		zassert_ok(k_sem_take(&worker_continue, K_MSEC(100)));
	}
	if (!coast && atomic_cas(&fail_observer, 1, 0)) return -EIO;
	return __real_motor_control_observe(motor, ticks, measured, coast, out);
}

static void sent(const struct device *dev, size_t size, void *arg)
{
	uint8_t bytes[128];
	zassert_equal(uart_emul_get_tx_data(dev, bytes, sizeof(bytes)), size);
}

static void angle_frame(struct k_timer *timer)
{
	if (!report_angle) return;
	/* Technic CALIB mode 4: ignored calibration word, shaft in decidegrees. */
	shaft_angle = (shaft_angle + shaft_increment) % 3600;
	uint8_t data[] = {0xd4, 0, 0, 0, 0, 0x2b};
	data[3] = shaft_angle;
	data[4] = shaft_angle >> 8;
	data[5] ^= data[3] ^ data[4];
	zassert_equal(uart_emul_put_rx_data(uart, data, sizeof(data)), sizeof(data));
}
K_TIMER_DEFINE(encoder, angle_frame, NULL);

static int voltage(const struct device *dev, unsigned int channel, void *context, uint32_t *out)
{
	*out = battery_raw;
	return battery_error;
}

static int pwm(const struct device *dev, uint32_t channel,
	uint32_t period, uint32_t pulse, pwm_flags_t flags)
{
	if (pulse && block_pwm) {
		block_pwm = false;
		k_sem_give(&worker_entered);
		zassert_ok(k_sem_take(&worker_continue, K_MSEC(100)));
	}
	if (pulse && pwm_delay) {
		uint32_t delay = pwm_delay;
		pwm_delay = 0;
		k_msleep(delay);
	}
	return 0;
}

static struct bp_motor_state state(void)
{
	struct bp_motor_state value;
	zassert_ok(hub_motor_get(0, &value));
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

static struct bp_motor_state completed(void)
{
	int64_t deadline = k_uptime_get() + 100;
	for (;;) {
		struct bp_motor_state value = state();
		if (!value.pending) return value;
		zassert_true(k_uptime_get() < deadline, "Command stalled");
		k_msleep(1);
	}
}

static void ready(void);

static struct bp_motor_group_state group_settled(uint32_t request, bool active)
{
	int64_t deadline = k_uptime_get() + 150;
	struct bp_motor_group_state group;
	do {
		zassert_ok(hub_motor_group_get(&group));
		if (group.request == request && !group.pending && group.active == active) return group;
		zassert_true(k_uptime_get() < deadline, "Group %u did not settle", request);
		k_msleep(1);
	} while (true);
}

static struct bp_motor_group_command group_command(uint32_t generation_a, uint32_t generation_b,
	int32_t duty_a, int32_t duty_b, int64_t deadline)
{
	struct bp_motor_group_command command = {
		.port_mask = BIT(0) | BIT(1), .deadline_ms = deadline,
	};
	command.ports[0].generation = generation_a;
	command.ports[1].generation = generation_b;
	command.ports[0].duty_permille = duty_a;
	command.ports[1].duty_permille = duty_b;
	return command;
}

static struct bp_motor_observation observation_after(uint32_t generation, uint32_t frames,
	int64_t completed_ms)
{
	int64_t deadline = k_uptime_get() + 100;
	struct bp_motor_observation sample;
	do {
		zassert_ok(hub_motor_observation_get(0, generation, 20, &sample));
		if (!sample.error && sample.frames > frames && sample.sample_ms > completed_ms) return sample;
		zassert_true(k_uptime_get() < deadline, "Observer error %d, frames %u", sample.error, sample.frames);
		k_msleep(1);
	} while (true);
}

static void group_observation(uint32_t generation_a)
{
	struct bp_port_state port_b;
	zassert_ok(hub_port_get(1, &port_b));
	uint32_t observe_request, group_request;
	zassert_ok(hub_motor_observe(BP_PORT_APPLICATION, 0, generation_a, &observe_request));
	zassert_ok(completed().error);
	struct bp_motor_observation sample = observation_after(generation_a, 0, 0);
	struct bp_battery_sample battery;
	zassert_ok(hub_battery_get(&battery));
	zassert_ok(battery.error);
	struct bp_motor_group_command command = group_command(generation_a, port_b.generation,
		500, -400, k_uptime_get() + 300);
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &group_request));
	struct bp_motor_group_state group = group_settled(group_request, true);
	zassert_ok(group.error);
	zassert_equal(group.applied_mask, group.port_mask);
	sample = observation_after(generation_a, sample.frames, group.completed_ms);
	zassert_equal(sample.applied_mv, (int64_t)500 * battery.voltage_mv / 1000);
	zassert_true(state().control.observing);
	uint32_t frames = sample.frames;
	command = group_command(generation_a, port_b.generation, -600, 350, k_uptime_get() + 300);
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &group_request));
	group = group_settled(group_request, true);
	zassert_ok(group.error);
	sample = observation_after(generation_a, frames, group.completed_ms);
	zassert_equal(sample.applied_mv, (int64_t)-600 * battery.voltage_mv / 1000);
	zassert_true(sample.frames > frames, "Group replacement reset the observer");

	/* A model-frame failure coasts both members and retains the first error. */
	atomic_set(&fail_observer, 1);
	group = group_settled(group_request, false);
	zassert_equal(group.error, -EIO);
	zassert_equal(group.cleanup_mask, group.port_mask);
	struct bp_motor_state motor_b;
	zassert_ok(hub_motor_get(1, &motor_b));
	zassert_equal(state().mode, BP_MOTOR_COAST);
	zassert_equal(motor_b.mode, BP_MOTOR_COAST);
	zassert_equal(state().error, -EIO);
	zassert_ok(hub_motor_observation_get(0, generation_a, 20, &sample));
	zassert_equal(sample.error, -EIO);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	ready();

	/* Deadline expiry follows the same group-wide cleanup while observing. */
	zassert_ok(hub_motor_observe(BP_PORT_APPLICATION, 0, generation_a, &observe_request));
	zassert_ok(completed().error);
	sample = observation_after(generation_a, 0, 0);
	command = group_command(generation_a, port_b.generation, 250, -250, k_uptime_get() + 40);
	zassert_ok(hub_motor_group_set(BP_PORT_APPLICATION, &command, &group_request));
	group_settled(group_request, true);
	group = group_settled(group_request, false);
	zassert_equal(group.error, -ETIMEDOUT);
	zassert_equal(group.cleanup_mask, group.port_mask);
	zassert_ok(hub_motor_get(1, &motor_b));
	zassert_equal(state().mode, BP_MOTOR_COAST);
	zassert_equal(motor_b.mode, BP_MOTOR_COAST);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
}

static void ready(void)
{
	int64_t deadline = k_uptime_get() + 100;
	for (;;) {
		struct bp_sensor_sample sample;
		zassert_ok(hub_sensor_get(0, &sample));
		if (!sample.error) return;
		zassert_true(k_uptime_get() < deadline, "Encoder not ready: %d", sample.error);
		k_msleep(1);
	}
}

static void feed(const uint8_t *data, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		zassert_equal(uart_emul_put_rx_data(uart, data + i, 1), 1);
		k_msleep(5);
	}
}

static void connect_motor(void)
{
	uart_emul_callback_tx_data_ready_set(uart, sent, NULL);
	zassert_ok(adc_emul_const_raw_value_set(adc, 10, 0));
	zassert_ok(adc_emul_raw_value_func_set(adc, 11, voltage, NULL));
	fake_pwm_set_cycles_fake.custom_fake = pwm;
	pup_test_circuits[0].kind = UART_DEVICE;
	pup_test_circuits[1].kind = TRAIN_MOTOR;
	int64_t deadline = k_uptime_get() + 1000;
	struct bp_port_state port;
	do {
		k_msleep(1);
		zassert_ok(hub_port_get(0, &port));
		zassert_true(k_uptime_get() < deadline);
	} while (port.phase != BP_PORT_NEGOTIATING);
	uint8_t ack = 4;
	feed(&ack, 1);
	feed(technic_large_motor, sizeof(technic_large_motor));
	report_angle = true;
	k_timer_start(&encoder, K_MSEC(5), K_MSEC(5));
	deadline = k_uptime_get() + 1000;
	for (;;) {
		struct bp_sensor_sample sample;
		struct bp_battery_sample battery;
		zassert_ok(hub_sensor_get(0, &sample));
		zassert_ok(hub_battery_get(&battery));
		if (!sample.error && !battery.error) break;
		zassert_true(k_uptime_get() < deadline, "Sensor %d battery %d", sample.error, battery.error);
		k_msleep(1);
	}
	deadline = k_uptime_get() + 1000;
	do {
		zassert_ok(hub_port_get(1, &port));
		if (!port.error && port.type_id == LEGO_DEVICE_TYPE_ID_LPF2_TRAIN) break;
		zassert_true(k_uptime_get() < deadline, "Second motor not ready: %d", port.error);
		k_msleep(1);
	} while (true);
}

static struct bp_motor_command run(uint32_t duration, uint8_t finish)
{
	return (struct bp_motor_command){.mode = BP_MOTOR_SPEED, .speed_mdeg_s = 300000,
		.duration_ms = duration, .finish = finish};
}

static void observe_control(void)
{
	/* Exercise the registered handler and its actual transport-sized buffer. */
	static struct cbor_nb_reader reader;
	static struct cbor_nb_writer writer;
	static uint8_t response[CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - 8];
	static const uint8_t request[] = "\xa1\x66source\x69" "control.A";
	zcbor_new_decode_state(reader.zs, ARRAY_SIZE(reader.zs), request, sizeof(request) - 1, 1, NULL, 0);
	zcbor_new_encode_state(writer.zs, ARRAY_SIZE(writer.zs), response, sizeof(response), 1);
	zassert_true(zcbor_map_start_encode(writer.zs, 5));
	struct smp_streamer stream = {.reader = &reader, .writer = &writer};
	zassert_ok(mgmt_find_handler(66, 1)->mh_read(&stream));
	zassert_true(zcbor_map_end_encode(writer.zs, 5));
	zcbor_new_decode_state(reader.zs, ARRAY_SIZE(reader.zs), response,
		writer.zs[0].payload - response, 1, NULL, 0);
	zassert_true(zcbor_map_start_decode(reader.zs));
	bool found = false;
	while (!zcbor_array_at_end(reader.zs)) {
		struct zcbor_string key;
		zassert_true(zcbor_tstr_decode(reader.zs, &key));
		if (key.len != 6 || memcmp(key.value, "values", 6)) {
			zassert_true(zcbor_any_skip(reader.zs, NULL));
			continue;
		}
		zassert_true(zcbor_map_start_decode(reader.zs));
		while (!zcbor_array_at_end(reader.zs)) {
			int64_t number;
			zassert_true(zcbor_tstr_decode(reader.zs, &key));
			zassert_true(zcbor_int64_decode(reader.zs, &number));
			if (key.len == 7 && !memcmp(key.value, "stalled", 7)) {
				zassert_equal(number, 1);
				found = true;
			}
		}
		zassert_true(zcbor_map_end_decode(reader.zs));
	}
	zassert_true(found && zcbor_map_end_decode(reader.zs));
	zcbor_new_encode_state(writer.zs, ARRAY_SIZE(writer.zs), response, sizeof(response), 1);
	zassert_true(zcbor_map_start_encode(writer.zs, 5));
	zassert_ok(mgmt_find_handler(66, 0)->mh_read(&stream));
	zassert_true(zcbor_map_end_encode(writer.zs, 5));
}

ZTEST(regulated_motors, test_control_lifetime)
{
	connect_motor();
	/* Ordinary first use: observe without first starting a trajectory. Idle
	 * frames must not repeatedly write PWM or reset the derivative history. */
	struct bp_port_state port;
	zassert_ok(hub_port_get(0, &port));
	uint32_t observe_request;
	size_t heap_before_control = allocated_bytes();
	struct bp_motor_state before = state();
	unsigned int pwm_before = fake_pwm_set_cycles_fake.call_count;
	/* Force first-use allocation failure before ownership or output changes. */
	struct heap_filler { struct heap_filler *next; };
	struct heap_filler *fillers = NULL, *block;
	while ((block = k_malloc(64)) != NULL) {
		block->next = fillers;
		fillers = block;
	}
	zassert_not_null(fillers);
	zassert_equal(hub_motor_observe(BP_PORT_APPLICATION, 0, port.generation, &observe_request), -ENOMEM);
	struct bp_motor_state rejected = state();
	zassert_equal(rejected.owner, before.owner);
	zassert_equal(rejected.request, before.request);
	zassert_equal(rejected.pending, before.pending);
	zassert_equal(rejected.duty, before.duty);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, pwm_before);
	while (fillers) {
		block = fillers;
		fillers = block->next;
		k_free(block);
	}
	zassert_equal(allocated_bytes(), heap_before_control);
	zassert_ok(hub_motor_observe(BP_PORT_APPLICATION, 0, port.generation, &observe_request));
	zassert_ok(completed().error);
	zassert_true(allocated_bytes() > heap_before_control);
	struct bp_motor_observation observation;
	zassert_ok(hub_motor_observation_get(0, port.generation, 100, &observation));
	zassert_ok(observation.error);
	zassert_equal(observation.speed_mdeg_s, 0);
	/* A coasting observer leaves output valid, but close must retire its
	 * frame before a new owner can reuse the controller. */
	atomic_set(&block_observer, 1);
	zassert_ok(k_sem_take(&worker_entered, K_MSEC(100)));
	struct bp_motor_state passive = state();
	zassert_equal(passive.mode, BP_MOTOR_COAST);
	zassert_true(passive.valid && !passive.pending && passive.control.observing);
	size_t heap_during_observer = allocated_bytes();
	zassert_true(heap_during_observer > heap_before_control);
	zassert_ok(hub_port_release(BP_PORT_APPLICATION, 0));
	zassert_equal(state().owner, BP_PORT_APPLICATION);
	zassert_equal(hub_motor_observe(BP_PORT_TERMINAL, 0, port.generation, &observe_request), -EBUSY);
	zassert_equal(allocated_bytes(), heap_during_observer,
		"Release freed a controller still used by the passive frame");
	k_sem_give(&worker_continue);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	zassert_equal(allocated_bytes(), heap_before_control);
	ready();
	group_observation(port.generation);
	ready();
	zassert_equal(allocated_bytes(), heap_before_control);
	zassert_ok(hub_motor_observe(BP_PORT_APPLICATION, 0, port.generation, &observe_request));
	zassert_ok(completed().error);
	zassert_ok(hub_motor_observation_get(0, port.generation, 100, &observation));
	zassert_ok(observation.error);
	unsigned int writes = fake_pwm_set_cycles_fake.call_count;
	uint32_t frames = observation.frames;
	shaft_increment = 10; /* 1 degree per 5 ms; independent synthetic motion. */
	k_msleep(130);
	zassert_ok(hub_motor_observation_get(0, port.generation, 100, &observation));
	zassert_ok(observation.error);
	zassert_true(observation.frames >= frames + 24);
	zassert_within(observation.speed_mdeg_s, 200000, 20000);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, writes, "Idle observation rewrote PWM");
	zassert_equal(observation.load_mnm, 0);
	zassert_false(observation.stalled);
	zassert_equal(hub_motor_observation_get(0, port.generation, UINT32_MAX, &observation), -EINVAL);
	shaft_increment = 0;
	zassert_ok(hub_motor_drive(BP_PORT_APPLICATION, 0, 800, 0));
	zassert_ok(completed().error);
	/* Wait for the model to lose its previous motion, then satisfy the 200 ms
	 * stall gate; that gate is not a deadline measured from the DC command. */
	int64_t deadline = k_uptime_get() + 1000;
	do {
		k_msleep(5);
		zassert_ok(hub_motor_observation_get(0, port.generation, 20, &observation));
		zassert_ok(observation.error);
		zassert_true(k_uptime_get() < deadline, "Model did not settle into stall");
	} while (!observation.stalled);
	zassert_equal(observation.speed_mdeg_s, 0);
	zassert_true(observation.stalled && observation.load_mnm < 0,
		"stalled=%u load=%d estimated=%d voltage=%d frames=%u", observation.stalled,
		observation.load_mnm, observation.estimated_speed, observation.applied_mv, observation.frames);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	zassert_false(state().control.observing);
	zassert_ok(hub_motor_observation_get(0, port.generation, 100, &observation));
	zassert_equal(observation.error, -ENODEV);
	zassert_ok(hub_motor_observation_get(0, port.generation + 1, 100, &observation));
	zassert_equal(observation.error, -ESTALE);
	/* Missing measurements must stop even a coasting observer; otherwise its
	 * stale zero derivative could appear to be a live, stationary shaft. */
	ready(); /* Release restores automatic mode selection before a new claim. */
	zassert_ok(hub_motor_observe(BP_PORT_APPLICATION, 0, port.generation, &observe_request));
	zassert_ok(completed().error);
	report_angle = false;
	k_msleep(110);
	zassert_false(state().control.observing);
	zassert_ok(hub_motor_observation_get(0, port.generation, 100, &observation));
	zassert_equal(observation.error, -ETIMEDOUT);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	report_angle = true;
	shaft_angle = 0;
	deadline = k_uptime_get() + 100;
	for (;;) {
		struct bp_sensor_sample sample;
		zassert_ok(hub_sensor_get(0, &sample));
		if (!sample.error && sample.angle_mdeg == 0) break;
		zassert_true(k_uptime_get() < deadline);
		k_msleep(1);
	}
	struct bp_motor_command command = run(800, BP_MOTOR_FINISH_HOLD);
	zassert_ok(hub_motor_control(BP_PORT_APPLICATION, 0, &command));
	struct bp_motor_state value = completed();
	zassert_ok(value.error);
	zassert_true(value.control.active && value.valid);
	zassert_equal(value.owner, BP_PORT_APPLICATION);
	zassert_true(value.duty > 0 && value.control.applied_mv <= value.control.requested_mv);
	struct bp_motor_state second_motor;
	zassert_ok(hub_motor_get(1, &second_motor));
	struct bp_motor_group_command rejected_group = group_command(port.generation,
		second_motor.generation, 100, -100, k_uptime_get() + 100);
	struct bp_motor_group_state before_group, after_group;
	zassert_ok(hub_motor_group_get(&before_group));
	uint32_t rejected_request = 0;
	zassert_equal(hub_motor_group_set(BP_PORT_APPLICATION, &rejected_group, &rejected_request), -EBUSY);
	zassert_ok(hub_motor_group_get(&after_group));
	zassert_equal(after_group.request, before_group.request);
	zassert_equal(state().request, value.request);
	zassert_ok(hub_motor_get(1, &second_motor));
	zassert_equal(second_motor.owner, BP_PORT_UNOWNED);
	zassert_equal(hub_motor_control(BP_PORT_TERMINAL, 0, &command), -EBUSY);
	uint32_t request = value.request;
	command.speed_mdeg_s = INT32_MAX;
	zassert_equal(hub_motor_control(BP_PORT_APPLICATION, 0, &command), -EINVAL);
	zassert_equal(state().request, request, "Invalid command replaced motion");
	command = (struct bp_motor_command){.mode = BP_MOTOR_POSITION,
		.target_mdeg = 1000000000, .speed_mdeg_s = 1000,
		.finish = BP_MOTOR_FINISH_HOLD};
	zassert_equal(hub_motor_control(BP_PORT_APPLICATION, 0, &command), -ERANGE);
	command.target_mdeg = 360000;
	command.speed_mdeg_s = 1;
	zassert_equal(hub_motor_control(BP_PORT_APPLICATION, 0, &command), -ERANGE);
	zassert_equal(state().request, request, "Infeasible trajectory replaced motion");
	/* Allow the acceleration ramp and error buildup before the model's
	 * 200 ms stall persistence criterion can be met. */
	k_msleep(600);
	value = state();
	zassert_ok(value.error);
	zassert_true(value.control.stalled && value.control.frames >= 120,
		"stalled=%u frames=%u", value.control.stalled, value.control.frames);
	zassert_false(value.control.done);
	observe_control();
	/* Closing the computer's session does not stop an application's motor. */
	hub_ports_session_closed(BP_PORT_TERMINAL);
	zassert_true(state().control.active);
	k_msleep(220);
	value = state();
	zassert_ok(value.error);
	zassert_true(value.control.done && value.control.active);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	value = state();
	zassert_equal(value.owner, BP_PORT_UNOWNED);
	zassert_equal(value.mode, BP_MOTOR_COAST);
	zassert_false(value.control.active);
	zassert_equal(pup_test_sensor_supply(0), 0);
	ready();

	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_equal(shell_execute_cmd(shell, "motor A target 9223372036854775808 100000 hold"), -EINVAL);
	zassert_ok(shell_execute_cmd(shell, "motor A target 0 100000 hold"));
	value = completed();
	zassert_ok(value.error);
	zassert_true(value.control.active && value.control.done);
	zassert_ok(shell_execute_cmd(shell, "observe control.A"));
	zassert_ok(shell_execute_cmd(shell, "motor A run -300000 20 coast"));
	value = completed();
	zassert_ok(value.error);
	k_msleep(30);
	zassert_true(state().control.done);
	zassert_false(state().control.active);
	zassert_equal(state().mode, BP_MOTOR_COAST);
	hub_ports_session_closed(BP_PORT_TERMINAL);
	zassert_equal(completed().owner, BP_PORT_UNOWNED);
	ready();

	command = run(0, BP_MOTOR_FINISH_CONTINUE);
	zassert_ok(hub_motor_control(BP_PORT_APPLICATION, 0, &command));
	zassert_ok(completed().error);
	report_angle = false;
	k_msleep(110);
	value = state();
	zassert_equal(value.error, -ETIMEDOUT);
	zassert_true(value.valid && !value.control.active);
	zassert_equal(value.mode, BP_MOTOR_COAST);
	report_angle = true;
	k_msleep(10);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	ready();

	/* An ADC failure must not silently substitute a nominal battery voltage. */
	zassert_ok(hub_motor_control(BP_PORT_APPLICATION, 0, &command));
	zassert_ok(completed().error);
	battery_error = -EIO;
	k_msleep(25);
	value = state();
	zassert_equal(value.error, -EIO);
	zassert_true(value.valid && !value.control.active);
	zassert_equal(value.mode, BP_MOTOR_COAST);
	battery_error = 0;
	k_msleep(20);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	ready();

	/* Lose a whole frame before service runs; never integrate catch-up steps. */
	zassert_ok(hub_motor_control(BP_PORT_APPLICATION, 0, &command));
	zassert_ok(completed().error);
	k_sched_lock();
	k_busy_wait(12000);
	k_sched_unlock();
	k_msleep(1);
	value = state();
	zassert_equal(value.error, -ETIMEDOUT);
	zassert_equal(value.control.overruns, 1);
	zassert_equal(value.mode, BP_MOTOR_COAST);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	ready();

	/* A real blocking driver call crosses the next frame, requiring coast. */
	pwm_delay = 10;
	zassert_ok(hub_motor_control(BP_PORT_APPLICATION, 0, &command));
	value = completed();
	zassert_equal(value.error, -ETIMEDOUT);
	zassert_equal(value.control.overruns, 2);
	zassert_true(value.valid && !value.control.active);
	zassert_equal(value.mode, BP_MOTOR_COAST);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	ready();

	/* Release accepted during a PWM write retains ownership until coast. */
	size_t heap_before_blocked_control = allocated_bytes();
	block_pwm = true;
	zassert_ok(hub_motor_control(BP_PORT_APPLICATION, 0, &command));
	zassert_ok(k_sem_take(&worker_entered, K_MSEC(100)));
	size_t heap_during_blocked_control = allocated_bytes();
	zassert_true(heap_during_blocked_control > heap_before_blocked_control);
	zassert_ok(hub_port_release(BP_PORT_APPLICATION, 0));
	zassert_equal(state().owner, BP_PORT_APPLICATION);
	zassert_equal(allocated_bytes(), heap_during_blocked_control);
	zassert_equal(hub_motor_control(BP_PORT_TERMINAL, 0, &command), -EBUSY);
	k_sem_give(&worker_continue);
	value = completed();
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	zassert_equal(allocated_bytes(), heap_before_blocked_control);
	zassert_equal(value.owner, BP_PORT_UNOWNED);
	zassert_false(value.control.active);
	zassert_equal(value.mode, BP_MOTOR_COAST);
	zassert_equal(hub_motor_activity() & 1, 0);
	k_timer_stop(&encoder);
	zassert_ok(hub_ports_shutdown());
	hub_battery_shutdown();
}

ZTEST_SUITE(regulated_motors, NULL, NULL, NULL, NULL, NULL);
