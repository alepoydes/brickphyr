// SPDX-License-Identifier: Apache-2.0
/* Synthetic UART encoder and ADC, real port owner/controller/output path.
 * The shaft stays fixed: this checks commands, stall and cleanup, not tuning. */
#include "hub_ports.h"
#include "hub_battery.h"
#include "wiring.h"
#include "../lump/captures.h"
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

DEFINE_FFF_GLOBALS;
static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(lump_uart));
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
static bool report_angle;
static int16_t shaft_angle, shaft_increment;
static int battery_error;
static uint32_t battery_raw = 3100;
static uint32_t pwm_delay;
static bool block_pwm;
static K_SEM_DEFINE(pwm_entered, 0, 1);
static K_SEM_DEFINE(pwm_continue, 0, 1);

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
		k_sem_give(&pwm_entered);
		zassert_ok(k_sem_take(&pwm_continue, K_MSEC(100)));
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
	zassert_ok(hub_motor_observe(BP_PORT_APPLICATION, 0, port.generation, &observe_request));
	zassert_ok(completed().error);
	struct bp_motor_observation observation;
	zassert_ok(hub_motor_observation_get(0, port.generation, 100, &observation));
	zassert_ok(observation.error);
	zassert_equal(observation.speed_mdeg_s, 0);
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
	block_pwm = true;
	zassert_ok(hub_motor_control(BP_PORT_APPLICATION, 0, &command));
	zassert_ok(k_sem_take(&pwm_entered, K_MSEC(100)));
	zassert_ok(hub_port_release(BP_PORT_APPLICATION, 0));
	zassert_equal(state().owner, BP_PORT_APPLICATION);
	zassert_equal(hub_motor_control(BP_PORT_TERMINAL, 0, &command), -EBUSY);
	k_sem_give(&pwm_continue);
	value = completed();
	zassert_equal(value.owner, BP_PORT_UNOWNED);
	zassert_false(value.control.active);
	zassert_equal(value.mode, BP_MOTOR_COAST);
	zassert_equal(hub_motor_activity() & 1, 0);
	k_timer_stop(&encoder);
	zassert_ok(hub_ports_shutdown());
	hub_battery_shutdown();
}

ZTEST_SUITE(regulated_motors, NULL, NULL, NULL, NULL, NULL);
