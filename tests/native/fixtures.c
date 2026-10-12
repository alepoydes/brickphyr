// SPDX-License-Identifier: Apache-2.0
#include "application.h"

#include <errno.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <brickphyr_abi.h>

static const unsigned char hello[] = {
#include <native_hello.inc>
};
static const unsigned char counter[] = {
#include <native_counter.inc>
};
static const unsigned char spin[] = {
#include <native_spin.inc>
};
static const unsigned char fault[] = {
#include <native_fault.inc>
};
static const unsigned char matrix[] = {
#include <native_matrix.inc>
};
static const unsigned char matrix_fault[] = {
#include <native_matrix_fault.inc>
};
static const unsigned char light_fault[] = {
#include <native_light_fault.inc>
};
static const unsigned char imu[] = {
#include <native_imu.inc>
};
static const unsigned char sound[] = {
#include <native_sound.inc>
};
static const unsigned char sound_fault[] = {
#include <native_sound_fault.inc>
};
static const unsigned char battery_fault[] = {
#include <native_battery_fault.inc>
};
static const unsigned char motor[] = {
#include <native_motor.inc>
};

const void *native_fixture_hello(size_t *length)
{
	*length = sizeof(hello);
	return hello;
}

int native_fixture_run(const char *name)
{
	if (!strcmp(name, "badabi")) {
		unsigned char *changed = k_malloc(sizeof(hello));
		if (!changed) {
			return -ENOMEM;
		}
		memcpy(changed, hello, sizeof(hello));
		int err = -ENOENT;
		for (size_t i = 0; i + sizeof(BRICKPHYR_ABI) <= sizeof(hello); i++) {
			if (!memcmp(changed + i, BRICKPHYR_ABI, sizeof(BRICKPHYR_ABI))) {
				changed[i] ^= 1;
				err = application_run_native(changed, sizeof(hello));
				break;
			}
		}
		k_free(changed);
		return err;
	}
	static const struct {
		const char *name;
		const void *data;
		size_t length;
	} images[] = {
		{"hello", hello, sizeof(hello)}, {"counter", counter, sizeof(counter)},
		{"spin", spin, sizeof(spin)}, {"fault", fault, sizeof(fault)},
		{"matrix", matrix, sizeof(matrix)}, {"matrix_fault", matrix_fault, sizeof(matrix_fault)},
		{"light_fault", light_fault, sizeof(light_fault)},
		{"imu", imu, sizeof(imu)},
		{"sound", sound, sizeof(sound)}, {"sound_fault", sound_fault, sizeof(sound_fault)},
		{"battery_fault", battery_fault, sizeof(battery_fault)},
		{"motor", motor, sizeof(motor)},
		{"truncated", hello, sizeof(hello) - 1},
	};
	for (size_t i = 0; i < ARRAY_SIZE(images); i++) {
		if (!strcmp(name, images[i].name)) {
			return application_run_native(images[i].data, images[i].length);
		}
	}
	return -EINVAL;
}

static int cmd_fixture(const struct shell *shell, size_t argc, char **argv)
{
	int err = native_fixture_run(argv[1]);
	shell_print(shell, "Load %s: %d", argv[1], err);
	return err;
}

SHELL_CMD_ARG_REGISTER(native_fixture, NULL, "Board test: hello, counter, spin, fault, matrix, matrix_fault, light_fault, imu, sound, sound_fault, battery_fault, motor, truncated, badabi.",
		      cmd_fixture, 2, 0);
