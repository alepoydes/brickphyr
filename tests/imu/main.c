// SPDX-License-Identifier: Apache-2.0
/* Synthetic register peer. Exercises the unmodified ST/Pybricks helpers,
 * Zephyr I2C/GPIO boundary and actual acquisition thread, not board timing. */
#include <string.h>
#include <math.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/settings/settings.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <pbio/imu.h>
#include "hub_imu.h"
#include "hub_power.h"
#include "configuration.h"
#include "storage.h"
#include "hub_ports.h"
#include "../ports/wiring.h"
#include <zephyr/fff.h>

DEFINE_FFF_GLOBALS;

static uint8_t registers[256];
static unsigned int transactions;
static bool fail_read;
static bool hold_read;
static K_SEM_DEFINE(entered, 0, 1);
static K_SEM_DEFINE(resume, 0, 1);

/* Raw register values for the same physical readings on each hub mounting. */
static const struct {
	int16_t still[6];
	int16_t biased[6];
	int16_t rotating[6];
	int16_t first[6];
} raw_frames = {
#ifdef IMU_TECHNIC_MOUNT
	.still = {10, -20, 30, 0, 0, 4096},
	.biased = {0, 0, 100, 0, 0, 4096},
	.rotating = {0, 0, 1000, 0, 0, 4096},
	.first = {INT16_MIN, -1000, 1000, 0, -1000, 4098},
#else
	.still = {10, 20, -30, 0, 0, -4096},
	.biased = {0, 0, -100, 0, 0, -4096},
	.rotating = {0, 0, -1000, 0, 0, -4096},
	.first = {INT16_MIN, 1000, -1000, 0, 1000, -4098},
#endif
};

static void pulse(void)
{
	const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	zassert_ok(gpio_emul_input_set(gpio, 4, 0));
	zassert_ok(gpio_emul_input_set(gpio, 4, 1));
	zassert_ok(gpio_emul_input_set(gpio, 4, 0));
}

static int transfer(const struct emul *target, struct i2c_msg *msgs, int count, int address)
{
	transactions++;
	zassert_equal(address, 0x6a);
	zassert_true(count == 1 || count == 2);
	zassert_true(count == 1 ? msgs[0].len > 1 : msgs[0].len == 1);
	uint8_t reg = msgs[0].buf[0];
	bool read = count == 2 && (msgs[1].flags & I2C_MSG_READ);
	uint8_t *data = count == 1 ? msgs[0].buf + 1 : msgs[1].buf;
	size_t size = count == 1 ? msgs[0].len - 1 : msgs[1].len;
	zassert_true((size_t)reg + size <= sizeof(registers));
#ifdef IMU_INIT_FAILURE
	if (reg == 0x12 && read) {
		return -EACCES;
	}
	if (reg == 0x11 && !read) {
		return -ETIMEDOUT; /* Cleanup must preserve the configuration error. */
	}
#endif
	if (read) {
		if (reg == 0x22 && hold_read) {
			hold_read = false;
			k_sem_give(&entered);
			zassert_ok(k_sem_take(&resume, K_SECONDS(1)));
		}
		memcpy(data, registers + reg, size);
		if (reg == 0x22 && fail_read) {
			fail_read = false;
			return -EIO;
		}
	} else {
		memcpy(registers + reg, data, size);
		if (reg == 0x12 && (registers[reg] & 1)) {
			memset(registers, 0, sizeof(registers));
			registers[0x0f] = 0x6a;
			registers[0x12] = 4; /* Reset default: address auto-increment. */
		}
	}
	return 0;
}

static int initialize(const struct emul *target, const struct device *bus)
{
	registers[0x0f] = 0x6a;
	registers[0x12] = 4;
	return 0;
}

static const struct i2c_emul_api api = {.transfer = transfer};
/* The emulator framework associates a device object with every bus peer.
 * Production acquisition consumes this node's bus/GPIO descriptors directly. */
DEVICE_DT_DEFINE(DT_NODELABEL(hub_imu), NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL);
EMUL_DT_DEFINE(DT_NODELABEL(hub_imu), initialize, NULL, NULL, &api, NULL);

static struct bp_imu_sample latest(uint32_t after, int error)
{
	struct bp_imu_sample value;
	int64_t deadline = k_uptime_get() + 100;
	do {
		zassert_ok(hub_imu_get(&value));
		if (value.sequence > after || (error && value.error == error)) {
			return value;
		}
		k_usleep(100);
	} while (k_uptime_get() < deadline);
	zassert_unreachable("No sample/error: sequence %u error %d", value.sequence, value.error);
	return value;
}

static struct bp_imu_state frame(const int16_t data[6])
{
	struct bp_imu_state state;
	zassert_ok(hub_imu_get_state(&state));
	uint32_t previous = state.raw.sequence;
	for (size_t i = 0; i < 6; i++) {
		sys_put_le16(data[i], registers + 0x22 + i * 2);
	}
	k_usleep(1200);
	pulse();
	int64_t deadline = k_uptime_get() + 100;
	do {
		k_usleep(100);
		zassert_ok(hub_imu_get_state(&state));
		zassert_true(k_uptime_get() < deadline, "No processed sample: %d", state.error);
	} while (state.raw.sequence == previous);
	zassert_ok(state.error);
	return state;
}

static void smp_observation(const char *name, const char *field, int64_t expected,
	int expected_error)
{
	/* Exercise the registered production group with the actual SMP payload
	 * capacity. Transport framing/ownership is checked by tests/host. */
	static struct cbor_nb_reader reader;
	static struct cbor_nb_writer writer;
	static uint8_t request[40], response[CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - 8];
	zcbor_new_encode_state(writer.zs, ARRAY_SIZE(writer.zs), request, sizeof(request), 1);
	zassert_true(zcbor_map_start_encode(writer.zs, 1));
	zassert_true(zcbor_tstr_put_lit(writer.zs, "source"));
	zassert_true(zcbor_tstr_encode_ptr(writer.zs, name, strlen(name)));
	zassert_true(zcbor_map_end_encode(writer.zs, 1));
	zcbor_new_decode_state(reader.zs, ARRAY_SIZE(reader.zs), request,
		writer.zs[0].payload - request, 1, NULL, 0);
	zcbor_new_encode_state(writer.zs, ARRAY_SIZE(writer.zs), response, sizeof(response), 1);
	zassert_true(zcbor_map_start_encode(writer.zs, 5));
	struct smp_streamer stream = {.reader = &reader, .writer = &writer};
	const struct mgmt_handler *handler = mgmt_find_handler(66, 1);
	zassert_not_null(handler);
	int result = handler->mh_read(&stream);
	zassert_ok(result, "%s: SMP error %d, %u response bytes", name, result,
		(unsigned int)(writer.zs[0].payload - response));
	zassert_true(zcbor_map_end_encode(writer.zs, 5));
	zcbor_new_decode_state(reader.zs, ARRAY_SIZE(reader.zs), response,
		writer.zs[0].payload - response, 1, NULL, 0);
	zassert_true(zcbor_map_start_decode(reader.zs));
	bool found = false;
	bool found_error = false;
	while (!zcbor_array_at_end(reader.zs)) {
		struct zcbor_string key;
		zassert_true(zcbor_tstr_decode(reader.zs, &key));
		if (key.len == 5 && !memcmp(key.value, "error", 5)) {
			int64_t error;
			zassert_true(zcbor_int64_decode(reader.zs, &error));
			zassert_equal(error, expected_error);
			found_error = true;
			continue;
		}
		if (key.len != 6 || memcmp(key.value, "values", 6)) {
			zassert_true(zcbor_any_skip(reader.zs, NULL));
			continue;
		}
		zassert_true(zcbor_map_start_decode(reader.zs));
		while (!zcbor_array_at_end(reader.zs)) {
			int64_t value;
			zassert_true(zcbor_tstr_decode(reader.zs, &key));
			zassert_true(zcbor_int64_decode(reader.zs, &value));
			if (expected_error == -ERANGE) {
				zassert_false(key.len == 13 && !memcmp(key.value, "heading1_mdeg", 13));
				zassert_false(key.len == 13 && !memcmp(key.value, "heading3_mdeg", 13));
			}
			if (field && key.len == strlen(field) && !memcmp(key.value, field, key.len)) {
				zassert_equal(value, expected);
				found = true;
			}
		}
		zassert_true(zcbor_map_end_decode(reader.zs));
	}
	zassert_true(zcbor_map_end_decode(reader.zs));
	zassert_true(found_error);
	if (field) zassert_true(found);
}

static void heading_observations(void)
{
	/* Set the production estimator's physical heading, then publish it via
	 * an actual still IMU frame before reading the registered SMP group. */
	static const float headings[] = {
		0x1.fffffep-5f, 0x1p-4f, 0x1.000002p-4f,
		-0x1.fffffep-5f, -0x1p-4f, -0x1.000002p-4f,
		0.000499999f, -0.000499999f,
		16777216.0f, -16777216.0f,
		0x1p52f, -0x1p52f,
	};
	for (size_t i = 0; i < ARRAY_SIZE(headings); i++) {
		pbio_imu_set_heading(headings[i]);
		struct bp_imu_state state = frame(raw_frames.still);
		zassert_equal(state.heading_1d, headings[i]);
		zassert_equal(state.heading_3d, headings[i]);
		smp_observation("imu.orientation", "heading1_mdeg",
			llround((double)state.heading_1d * 1000), 0);
		state = frame(raw_frames.still);
		smp_observation("imu.orientation", "heading3_mdeg",
			llround((double)state.heading_3d * 1000), 0);
	}
	pbio_imu_set_heading(0x1p54f);
	(void)frame(raw_frames.still);
	smp_observation("imu.orientation", NULL, 0, -ERANGE);
	pbio_imu_set_heading(0.0f);
	(void)frame(raw_frames.still);
}

static void processing(void)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	zassert_ok(storage_status());
	zassert_ok(settings_save());
	zassert_ok(shell_execute_cmd(shell, "config set imu/gyro_bias_start_mdps -700 1400 2100"));
	struct bp_imu_state state = frame(raw_frames.still);
	zassert_equal(state.bias[0], -700);
	zassert_equal(state.bias[1], 1400);
	zassert_equal(state.bias[2], 2100);
	for (size_t i = 0; i < 3; i++) {
		zassert_equal(state.angular_velocity[i], 0);
		zassert_within(state.orientation[4 * i], 1, 0.00001f);
	}
	zassert_false(state.ready);
	heading_observations();
	uint8_t saved[24], effective[24];
	zassert_equal(settings_load_one(imu_config[2].name, saved, sizeof(saved)), 12);
	zassert_equal(sys_get_le32(saved), 0, "RAM update wrote storage");
	zassert_ok(shell_execute_cmd(shell, "config save"));
	zassert_equal(settings_load_one(imu_config[2].name, saved, sizeof(saved)), 12);
	zassert_equal((int32_t)sys_get_le32(saved), -700);
	zassert_ok(shell_execute_cmd(shell, "config set imu/gyro_bias_start_mdps 0 0 0"));
	zassert_ok(config_load(imu_config, ARRAY_SIZE(imu_config)));
	state = frame(raw_frames.still);
	zassert_equal(state.angular_velocity[2], 0, "Loaded bias was not applied");
	zassert_equal(state.bias[2], 2100);
	uint32_t epoch = state.epoch;
	zassert_equal(shell_execute_cmd(shell, "config set imu/gyro_scale_mdeg 360000 1 361000"), -EINVAL);
	zassert_equal(settings_runtime_get(imu_config[3].name, effective, sizeof(effective)), 12);
	for (size_t i = 0; i < 3; i++) {
		zassert_equal(sys_get_le32(effective + 4 * i), 360000);
	}
	zassert_equal(shell_execute_cmd(shell, "config set imu/gyro_bias_start_mdps 0 0"), -EINVAL);
	zassert_equal(shell_execute_cmd(shell, "config set imu/gyro_bias_start_mdps 0 0 -2147483648"), -EINVAL);
	zassert_equal(shell_execute_cmd(shell, "config set imu/gyro_stationary_mdps -1"), -EINVAL);
	zassert_equal(shell_execute_cmd(shell, "config set imu/accel_calibration_um_s2 9806650 9806650 9806650 -9806650 9806650 -9806650"), -EINVAL);
	state = frame(raw_frames.still);
	zassert_equal(state.epoch, epoch, "Rejected settings reset orientation");
	zassert_equal(state.angular_velocity[2], 0);
	zassert_ok(shell_execute_cmd(shell, "config set imu/gyro_stationary_mdps 2147483647"));
	zassert_equal(settings_runtime_get(imu_config[0].name, effective, sizeof(effective)), 4);
	zassert_equal(sys_get_le32(effective), INT32_MAX, "Selected integer was rounded through float");
	zassert_ok(shell_execute_cmd(shell, "config set imu/gyro_stationary_mdps 2000"));
	zassert_ok(shell_execute_cmd(shell, "config set power/usb_idle_s 4294967295"));
	zassert_equal(settings_runtime_get("power/usb_idle_s", effective, sizeof(effective)), 4);
	zassert_equal(sys_get_le32(effective), UINT32_MAX);
	zassert_ok(shell_execute_cmd(shell, "config set power/usb_idle_s 0"));

	/* A synthetic fixed offset: real acquisition, stationary detection and
	 * PBIO calibration must remove it without a storage write. */
	zassert_true(hub_power_application(true));
	pup_test_circuits[0].kind = MEDIUM_MOTOR;
	struct bp_motor_state motor;
	int64_t deadline = k_uptime_get() + 1000;
	do {
		k_msleep(1);
		zassert_ok(hub_motor_get(0, &motor));
		zassert_true(k_uptime_get() < deadline);
	} while (motor.error);
	zassert_ok(hub_motor_drive(BP_PORT_APPLICATION, 0, 250, 0));
	for (size_t i = 0; i < 2800; i++) state = frame(raw_frames.biased);
	zassert_false(state.stationary);
	zassert_equal(state.bias[2], 2100, "Motor operation was learned as gyro bias");
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	for (size_t i = 0; i < 2800; i++) {
		state = frame(raw_frames.biased);
	}
	zassert_true(state.stationary && state.ready);
	zassert_equal(state.bias[2], 7000);
	zassert_equal(state.angular_velocity[2], 0);
	zassert_equal(settings_load_one(imu_config[2].name, effective, sizeof(effective)), 12);
	zassert_mem_equal(effective, saved, 12, "Learning wrote flash");
	/* Even a complete motor pulse between IMU frames resets the stationary
	 * window. Looking only at the current duty would miss this transition. */
	zassert_ok(hub_motor_drive(BP_PORT_APPLICATION, 0, 250, 0));
	do {
		k_msleep(1);
		zassert_ok(hub_motor_get(0, &motor));
	} while (motor.pending);
	zassert_ok(hub_ports_release(BP_PORT_APPLICATION));
	state = frame(raw_frames.biased);
	zassert_false(state.stationary);
	zassert_equal(state.bias[2], 7000, "Inhibition erased the previous bias");

	/* Zero disables learning when measuring sustained rotation. Mere signal
	 * stability cannot distinguish rotation about gravity from a gyro bias. */
	zassert_ok(shell_execute_cmd(shell, "config set imu/gyro_stationary_mdps 0"));
	zassert_ok(shell_execute_cmd(shell, "config set imu/gyro_bias_start_mdps 0 0 0"));
	state = frame(raw_frames.rotating);
	int64_t start = state.raw.sample_us;
	epoch = state.epoch;
	for (size_t i = 0; i < 1000; i++) {
		state = frame(raw_frames.rotating);
	}
	float degrees = 70.0 * (state.raw.sample_us - start) / 1000000.0;
	zassert_equal(state.epoch, epoch);
	zassert_false(state.stationary);
	zassert_equal(state.bias[2], 0);
	zassert_within(state.rotation[2], degrees, 0.02f);
	zassert_within(state.heading_1d, -degrees, 0.02f);
	zassert_within(state.heading_3d, -degrees, 0.02f);
	zassert_within(state.orientation[0], cosf(degrees * 0.01745329252f), 0.001f);
	zassert_within(state.orientation[3], sinf(degrees * 0.01745329252f), 0.001f);
	unsigned int before = transactions;
	shell_backend_dummy_clear_output(shell);
	zassert_ok(shell_execute_cmd(shell, "observe imu.calibrated"));
	size_t length;
	const char *output = shell_backend_dummy_get_output(shell, &length);
	zassert_not_null(strstr(output, "gz_mdps=70000"));
	shell_backend_dummy_clear_output(shell);
	zassert_ok(shell_execute_cmd(shell, "observe imu.orientation"));
	output = shell_backend_dummy_get_output(shell, &length);
	zassert_not_null(strstr(output, "m33=1000000"));
	smp_observation("imu.calibrated", "gz_mdps", 70000, 0);
	smp_observation("imu.orientation", "m33", 1000000, 0);
	zassert_equal(transactions, before, "Observations triggered acquisition");
	k_msleep(6);
	zassert_ok(hub_imu_get_state(&state));
	zassert_equal(state.error, -ETIMEDOUT);
	zassert_false(state.ready);
	state = frame(raw_frames.rotating);
	zassert_equal(state.epoch, epoch + 1);
	zassert_within(state.rotation[2], 0, 0.00001f);
	zassert_true(hub_power_application(false));
}

ZTEST(imu, test_acquisition)
{
	int64_t deadline = k_uptime_get() + 200;
	struct bp_imu_sample initial;
	for (;;) {
		zassert_ok(hub_imu_get(&initial));
		if (initial.error != -EINPROGRESS) {
			break;
		}
		zassert_true(k_uptime_get() < deadline, "IMU did not initialize");
		k_usleep(100);
	}
#ifdef IMU_INIT_FAILURE
	zassert_equal(initial.error, -EACCES);
	zassert_equal(initial.sequence, 0);
	zassert_equal(initial.failures, 1);
	zassert_equal(registers[0x12], 4, "A failed read was followed by a write");
	unsigned int count = transactions;
	k_msleep(10);
	zassert_equal(transactions, count, "Failed initialization retried indefinitely");
	zassert_equal(hub_imu_shutdown(), -EACCES);
	return;
#endif
	/* Electrical configuration at the simulated chip, independently specified
	 * by the LSM6DS3TR-C register map: 833 Hz, 8g / 2000dps, BDU and INT1. */
	zassert_equal(registers[0x10], 0x7c);
	zassert_equal(registers[0x11], 0x7c);
	zassert_equal(registers[0x12] & 0x44, 0x44);
	zassert_equal(registers[0x13] & 8, 8);
	zassert_equal(registers[0x0b] & 0x80, 0x80);
	zassert_ok(hub_imu_get(&initial));
	zassert_equal(initial.sequence, 0);
	zassert_not_equal(initial.error, 0);
	unsigned int before = transactions;
	for (unsigned int i = 0; i < 50; i++) {
		zassert_ok(hub_imu_get(&initial));
	}
	zassert_equal(transactions, before, "Observation triggered I2C");
	for (size_t i = 0; i < ARRAY_SIZE(raw_frames.first); i++) {
		sys_put_le16(raw_frames.first[i], registers + 0x22 + i * 2);
	}
	pulse();
	struct bp_imu_sample first = latest(0, 0);
	zassert_ok(first.error);
	zassert_equal(first.angular_velocity[0], 2293760);
	zassert_equal(first.angular_velocity[1], 70000);
	zassert_equal(first.angular_velocity[2], 70000);
	zassert_equal(first.acceleration[0], 0);
	zassert_within(first.acceleration[1], 2393640, 2);
	/* 4098 counts represents approximately 1g at 8g full scale; allow half
	 * the sensor's quantization step, not a second copy of its conversion. */
	zassert_within(first.acceleration[2], 9810000, 1200);
	zassert_true(first.sample_us <= first.read_us);
	const struct shell *shell = shell_backend_dummy_get_ptr();
	shell_backend_dummy_clear_output(shell);
	before = transactions;
	zassert_ok(shell_execute_cmd(shell, "observe imu"));
	size_t length;
	const char *output = shell_backend_dummy_get_output(shell, &length);
	zassert_not_null(strstr(output, "gx_mdps=2293760"));
	zassert_not_null(strstr(output, "ax_um_s2=0"));
	zassert_equal(transactions, before, "Shell observation triggered acquisition");
	zassert_ok(hub_imu_get(&initial));
	zassert_equal(initial.sample_us, first.sample_us);
	k_msleep(6);
	zassert_ok(hub_imu_get(&initial));
	zassert_equal(initial.error, -ETIMEDOUT);
	zassert_equal(initial.sequence, first.sequence);
	zassert_equal(initial.sample_us, first.sample_us);

	/* I/O failure preserves data. One complete discarded read releases any
	 * partial word held by BDU; only the following frame becomes observable. */
	fail_read = true;
	pulse();
	struct bp_imu_sample value = latest(first.sequence, -EIO);
	zassert_equal(value.sequence, first.sequence);
	zassert_equal(value.sample_us, first.sample_us);
	zassert_equal(value.failures, 1);
	pulse();
	value = latest(first.sequence, -EAGAIN);
	zassert_equal(value.sequence, first.sequence);
	pulse();
	value = latest(first.sequence, 0);
	zassert_ok(value.error);
	zassert_equal(value.missed, 2);

	/* A newer ready edge during I/O invalidates the ambiguous frame. */
	uint32_t previous = value.sequence;
	hold_read = true;
	pulse();
	zassert_ok(k_sem_take(&entered, K_MSEC(100)));
	pulse();
	k_sem_give(&resume);
	value = latest(previous, 0);
	zassert_ok(value.error);
	zassert_equal(value.sequence, previous + 2);
	zassert_equal(value.missed, 3);
	zassert_equal(value.failures, 3);
	processing();
	zassert_ok(hub_imu_shutdown());
	zassert_equal(registers[0x10], 0);
	zassert_equal(registers[0x11], 0);
	before = transactions;
	pulse();
	k_msleep(2);
	zassert_equal(transactions, before);
	zassert_ok(hub_imu_get(&value));
	zassert_equal(value.error, -ESHUTDOWN);
}

ZTEST_SUITE(imu, NULL, NULL, NULL, NULL, NULL);
