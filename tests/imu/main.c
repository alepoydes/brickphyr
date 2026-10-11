// SPDX-License-Identifier: Apache-2.0
/* Synthetic register peer. Exercises the unmodified ST/Pybricks helpers,
 * Zephyr I2C/GPIO boundary and actual acquisition thread, not board timing. */
#include <string.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
#include <zephyr/shell/shell_dummy.h>
#include "hub_imu.h"

static uint8_t registers[256];
static unsigned int transactions;
static bool fail_read;
static bool hold_read;
static K_SEM_DEFINE(entered, 0, 1);
static K_SEM_DEFINE(resume, 0, 1);

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
	const int16_t raw[] = {INT16_MIN, 1000, -1000, 0, 1000, -4098};
	for (size_t i = 0; i < ARRAY_SIZE(raw); i++) {
		sys_put_le16(raw[i], registers + 0x22 + i * 2);
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
