// SPDX-License-Identifier: Apache-2.0
/* Synthetic I2C registers and GPIO edges. Acquisition, calibration, estimator,
 * snapshot publication and user-mode reads are the production implementations. */
#include <string.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include "hub_imu.h"
#include <math.h>

static uint8_t registers[256] = {[0x0f] = 0x6a, [0x12] = 4};
static int16_t input[6] = {0, 0, 0, 0, 0, -4096};
static struct k_spinlock lock;
static atomic_t fault;

static int transfer(const struct emul *emul, struct i2c_msg *msgs, int count, int address)
{
	__ASSERT_NO_MSG(address == 0x6a && (count == 1 || count == 2));
	uint8_t reg = msgs[0].buf[0];
	bool read = count == 2 && (msgs[1].flags & I2C_MSG_READ);
	uint8_t *data = count == 2 ? msgs[1].buf : msgs[0].buf + 1;
	size_t size = count == 2 ? msgs[1].len : msgs[0].len - 1;
	__ASSERT_NO_MSG((size_t)reg + size <= sizeof(registers));
	if (read && reg == 0x22 && atomic_get(&fault) == 1) return -EIO;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (read) {
		if (reg == 0x22) {
			for (size_t i = 0; i < ARRAY_SIZE(input); i++) {
				sys_put_le16(input[i], registers + reg + 2 * i);
			}
		}
		memcpy(data, registers + reg, size);
	} else {
		memcpy(registers + reg, data, size);
		if (reg == 0x12 && (registers[reg] & 1)) {
			memset(registers, 0, sizeof(registers));
			registers[0x0f] = 0x6a;
			registers[0x12] = 4;
		}
	}
	k_spin_unlock(&lock, key);
	return 0;
}
static int initialize(const struct emul *emul, const struct device *bus) { return 0; }
static const struct i2c_emul_api api = {.transfer = transfer};
DEVICE_DT_DEFINE(DT_NODELABEL(hub_imu), NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL);
EMUL_DT_DEFINE(DT_NODELABEL(hub_imu), initialize, NULL, NULL, &api, NULL);

static void edges(void *a, void *b, void *c)
{
	const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(imu_gpio));
	for (;;) {
		k_usleep(1200);
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool routed = registers[0x0d] & 2;
		k_spin_unlock(&lock, key);
		if (routed && atomic_get(&fault) != 2) {
			int high = gpio_emul_input_set(gpio, 0, 1);
			int low = gpio_emul_input_set(gpio, 0, 0);
			__ASSERT_NO_MSG(!high && !low);
		}
	}
}
K_THREAD_DEFINE(imu_peer, 1024, edges, NULL, NULL, NULL, 6, 0, 0);

static int set_input(const struct shell *shell, size_t argc, char **argv)
{
	int16_t values[6];
	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		int err = 0;
		long value = shell_strtol(argv[i + 1], 10, &err);
		if (err || value < INT16_MIN || value > INT16_MAX) return -EINVAL;
		values[i] = value;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	memcpy(input, values, sizeof(input));
	k_spin_unlock(&lock, key);
	return 0;
}
SHELL_CMD_ARG_REGISTER(imu_input, NULL, "Synthetic raw gyro/acceleration registers.", set_input, 7, 0);

static int set_fault(const struct shell *shell, size_t argc, char **argv)
{
	int err = 0;
	long value = shell_strtol(argv[1], 10, &err);
	if (err || value < 0 || value > 2) return -EINVAL;
	atomic_set(&fault, value);
	return 0;
}
SHELL_CMD_ARG_REGISTER(imu_error, NULL, "Synthetic fault: 0=none, 1=I2C, 2=no edges.", set_fault, 2, 0);

static int view_frame(const struct shell *shell, size_t argc, char **argv)
{
	struct bp_imu_application_state state;
	int err = hub_imu_get_application(&state);
	if (err) return err;
	shell_fprintf(shell, SHELL_NORMAL, "base:");
	for (size_t i = 0; i < ARRAY_SIZE(state.base); i++) {
		shell_fprintf(shell, SHELL_NORMAL, " %ld", lroundf(state.base[i] * 1000));
	}
	shell_print(shell, " pending:%u error:%d", state.pending, state.command_error);
	return 0;
}
SHELL_CMD_REGISTER(imu_view, NULL, "Inspect production application coordinates.", view_frame);
