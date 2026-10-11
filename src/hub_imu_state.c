// SPDX-License-Identifier: Apache-2.0
#include "hub_imu.h"
#include "hub_imu_internal.h"
#include "hub_ports.h"
#include "configuration.h"

#include <math.h>
#include <string.h>
#include <lsm6ds3tr_c_reg.h>
#include "imu/processing.h"
#include "imu/stationary.h"
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#define PERIOD_US DIV_ROUND_UP(1000000, 833)
#define STALE_US (4 * PERIOD_US)
static const int32_t signs[] = DT_PROP(DT_NODELABEL(hub_imu), axis_signs);

/* Only trusted processing/configuration code takes this mutex. Readers copy a
 * published snapshot under a separate short lock; applications own no lock. */
static K_MUTEX_DEFINE(processing_lock);
static struct k_spinlock snapshot_lock;
static struct bp_imu_state published = {.error = -EINPROGRESS, .raw.error = -EINPROGRESS};
static struct bp_imu_state current;
static pbio_imu_persistent_settings_t calibration;
static imu_stationary_t stationary;
static bool restart = true;
static int64_t last_us;
static uint32_t last_sequence;
static uint32_t motor_token;

enum setting { GYRO_THRESHOLD, ACCEL_THRESHOLD, BIAS, GYRO_SCALE, ACCEL_CALIBRATION, HEADING };
const struct config_entry imu_config[] = {
	{"imu/gyro_stationary_mdps", "mdps", 1, true},
	{"imu/accel_stationary_um_s2", "um/s2", 1, true},
	{"imu/gyro_bias_start_mdps", "mdps (initial bias; observe imu.calibrated for current bias)", 3, true},
	{"imu/gyro_scale_mdeg", "mdeg per turn", 3, true},
	{"imu/accel_calibration_um_s2", "um/s2 (+X +Y +Z -X -Y -Z)", 6, true},
	{"imu/heading_correction_mdeg", "mdeg per turn", 1, true},
};
/* Preserve the selected integers exactly, including values too large for a
 * float to represent every integer. PBIO owns their floating-point meaning. */
static int32_t selected[ARRAY_SIZE(imu_config)][6];

static void selected_from_calibration(void)
{
	selected[GYRO_THRESHOLD][0] = lroundf(calibration.gyro_stationary_threshold * 1000);
	selected[ACCEL_THRESHOLD][0] = lroundf(calibration.accel_stationary_threshold * 1000);
	selected[HEADING][0] = lroundf(calibration.heading_correction_1d * 1000);
	for (size_t i = 0; i < 3; i++) {
		selected[BIAS][i] = lroundf(calibration.angular_velocity_bias_start.values[i] * 1000);
		selected[GYRO_SCALE][i] = lroundf(calibration.angular_velocity_scale.values[i] * 1000);
		selected[ACCEL_CALIBRATION][i] = lroundf(calibration.gravity_pos.values[i] * 1000);
		selected[ACCEL_CALIBRATION][i + 3] = lroundf(calibration.gravity_neg.values[i] * 1000);
	}
}

static void publish(void)
{
	k_spinlock_key_t key = k_spin_lock(&snapshot_lock);
	published = current;
	k_spin_unlock(&snapshot_lock, key);
}

static int16_t raw_threshold(float threshold, float scale)
{
	float raw = threshold / scale;
	return raw <= 0 ? 0 : raw >= INT16_MAX ? INT16_MAX : MAX((int16_t)raw, 1);
}

void hub_imu_process(const struct bp_imu_sample *sample, const int16_t data[6])
{
	k_mutex_lock(&processing_lock, K_FOREVER);
	current.raw = *sample;
	if (sample->error) {
		restart = true;
		memset(&stationary, 0, sizeof(stationary));
		current.error = sample->error;
		current.stationary = current.ready = false;
		publish();
		k_mutex_unlock(&processing_lock);
		return;
	}

	/* Unobserved samples cannot be reconstructed by holding one gyro rate.
	 * Start a new reference and expose its epoch instead of hiding the gap. */
	int64_t elapsed = sample->sample_us - last_us;
	if (restart || sample->sequence - last_sequence != 1 || elapsed <= 0 || elapsed > STALE_US) {
		imu_processing_restart_orientation();
		memset(&stationary, 0, sizeof(stationary));
		current.epoch++;
		elapsed = 0;
		restart = false;
	}
	last_us = sample->sample_us;
	last_sequence = sample->sequence;

	uint32_t token = hub_motor_activity();
	if ((token & 1) || token != motor_token) memset(&stationary, 0, sizeof(stationary));
	motor_token = token;
	if (!(token & 1) && imu_stationary_update(&stationary, data,
		   raw_threshold(calibration.gyro_stationary_threshold,
			lsm6ds3tr_c_from_fs2000dps_to_mdps(1) / 1000.0f),
		   raw_threshold(calibration.accel_stationary_threshold,
			lsm6ds3tr_c_from_fs8g_to_mg(1) * 9.81f))) {
		pbio_geometry_xyz_t average;
		for (size_t i = 0; i < 3; i++) {
			average.values[i] = signs[i] * lsm6ds3tr_c_from_fs2000dps_to_mdps(
				(float)stationary.sum[i] / stationary.count) / 1000.0f;
		}
		/* A learned initial bias changes RAM only. Persistence is explicit. */
		if (imu_processing_stationary(&average, sample->sample_us / 1000)) {
			for (size_t i = 0; i < 3; i++) {
				selected[BIAS][i] = lroundf(calibration.angular_velocity_bias_start.values[i] * 1000);
			}
		}
	}

	pbio_geometry_xyz_t gyro, acceleration, bias, calibrated;
	for (size_t i = 0; i < 3; i++) {
		gyro.values[i] = sample->angular_velocity[i] / 1000.0f;
		acceleration.values[i] = sample->acceleration[i] / 1000.0f;
	}
	pbio_error_t error = imu_processing_sample(&gyro, &acceleration, elapsed / 1000000.0f);
	current.error = error == PBIO_SUCCESS ? 0 : error == PBIO_ERROR_AGAIN ? -EAGAIN : -EINVAL;
	/* A zero gravity vector cannot establish the initial pose. The next
	 * usable sample must start its reference without integrating the gap. */
	restart = current.error != 0;
	if (!current.error) {
		pbio_imu_get_acceleration(&calibrated, true);
		for (size_t i = 0; i < 3; i++) {
			current.acceleration[i] = lroundf(calibrated.values[i] * 1000);
		}
		pbio_imu_get_angular_velocity(&calibrated, true);
		imu_processing_get_bias(&bias);
		for (size_t i = 0; i < 3; i++) {
			current.angular_velocity[i] = lroundf(calibrated.values[i] * 1000);
			current.bias[i] = lroundf(bias.values[i] * 1000);
			pbio_geometry_xyz_t axis = {0};
			axis.values[i] = 1;
			pbio_imu_get_single_axis_rotation(&axis, &current.rotation[i], true);
		}
		pbio_geometry_matrix_3x3_t orientation;
		pbio_orientation_imu_get_orientation(&orientation);
		memcpy(current.orientation, orientation.values, sizeof(current.orientation));
		current.heading_1d = pbio_imu_get_heading(PBIO_IMU_HEADING_TYPE_1D);
		current.heading_3d = pbio_imu_get_heading(PBIO_IMU_HEADING_TYPE_3D);
	}
	current.stationary = stationary.stationary;
	current.ready = imu_processing_ready(sample->sample_us / 1000);
	publish();
	k_mutex_unlock(&processing_lock);
}

int hub_imu_get_state(struct bp_imu_state *state)
{
	if (!state) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&snapshot_lock);
	*state = published;
	k_spin_unlock(&snapshot_lock, key);
	int64_t reference = state->raw.sample_us ? state->raw.sample_us : state->raw.read_us;
	if ((!state->error || state->error == -EAGAIN) && reference &&
	    k_cyc_to_us_floor64(k_cycle_get_64()) - reference > STALE_US) {
		state->error = -ETIMEDOUT;
		state->stationary = state->ready = false;
	}
	return 0;
}

static int setting_get(const char *name, char *value, int capacity)
{
	int index = config_index(imu_config, ARRAY_SIZE(imu_config), name);
	if (index < 0) {
		return index;
	}
	size_t size = imu_config[index].count * sizeof(uint32_t);
	if (capacity < (int)size) {
		return -ENOSPC;
	}
	k_mutex_lock(&processing_lock, K_FOREVER);
	for (size_t i = 0; i < imu_config[index].count; i++) {
		sys_put_le32(selected[index][i], (uint8_t *)value + i * sizeof(uint32_t));
	}
	k_mutex_unlock(&processing_lock);
	return size;
}

static int setting_set(const char *name, size_t length, settings_read_cb read, void *context)
{
	int index = config_index(imu_config, ARRAY_SIZE(imu_config), name);
	if (index < 0 || length != imu_config[index].count * sizeof(uint32_t)) {
		return index < 0 ? index : -EINVAL;
	}
	uint8_t bytes[CONFIG_VALUE_BYTES];
	ssize_t received = read(context, bytes, length);
	if (received != length) {
		return received < 0 ? received : -EINVAL;
	}
	int32_t values[6];
	float physical[6];
	for (size_t i = 0; i < imu_config[index].count; i++) {
		values[i] = (int32_t)sys_get_le32(bytes + i * sizeof(uint32_t));
		physical[i] = values[i] / 1000.0f;
		/* An initial bias cannot exceed this chip's representable rate. */
		if (index == BIAS && fabsf(physical[i]) >
		    -lsm6ds3tr_c_from_fs2000dps_to_mdps(INT16_MIN) / 1000.0f) {
			return -EINVAL;
		}
	}
	pbio_imu_persistent_settings_t patch = {0};
	switch (index) {
	case GYRO_THRESHOLD:
		patch.flags = PBIO_IMU_SETTINGS_FLAGS_GYRO_STATIONARY_THRESHOLD_SET;
		patch.gyro_stationary_threshold = physical[0];
		break;
	case ACCEL_THRESHOLD:
		patch.flags = PBIO_IMU_SETTINGS_FLAGS_ACCEL_STATIONARY_THRESHOLD_SET;
		patch.accel_stationary_threshold = physical[0];
		break;
	case BIAS:
		patch.flags = PBIO_IMU_SETTINGS_FLAGS_GYRO_BIAS_INITIAL_SET;
		memcpy(patch.angular_velocity_bias_start.values, physical, 3 * sizeof(float));
		break;
	case GYRO_SCALE:
		patch.flags = PBIO_IMU_SETTINGS_FLAGS_GYRO_SCALE_SET;
		memcpy(patch.angular_velocity_scale.values, physical, 3 * sizeof(float));
		break;
	case ACCEL_CALIBRATION:
		patch.flags = PBIO_IMU_SETTINGS_FLAGS_ACCEL_CALIBRATED;
		memcpy(patch.gravity_pos.values, physical, 3 * sizeof(float));
		memcpy(patch.gravity_neg.values, physical + 3, 3 * sizeof(float));
		break;
	case HEADING:
		patch.flags = PBIO_IMU_SETTINGS_FLAGS_HEADING_CORRECTION_1D_SET;
		patch.heading_correction_1d = physical[0];
		break;
	}
	k_mutex_lock(&processing_lock, K_FOREVER);
	pbio_error_t err = imu_processing_set_settings(&patch);
	if (!err) {
		memcpy(selected[index], values, length);
		if (index == BIAS) {
			imu_processing_apply_settings(&calibration);
		}
		/* Changing calibration starts a new reference on the next frame;
		 * previously published angles never silently acquire a new scale. */
		restart = true;
		current.error = -EAGAIN;
		current.ready = current.stationary = false;
		publish();
	}
	k_mutex_unlock(&processing_lock);
	return err ? -EINVAL : 0;
}

static int setting_export(int (*save)(const char *, const void *, size_t))
{
	/* config_export copies each value before calling storage, outside our lock. */
	return config_export(imu_config, ARRAY_SIZE(imu_config), save);
}

SETTINGS_STATIC_HANDLER_DEFINE(imu, "imu", setting_get, setting_set, NULL, setting_export);

static int initialize(void)
{
	imu_processing_reset();
	imu_processing_default_settings(&calibration);
	imu_processing_apply_settings(&calibration);
	selected_from_calibration();
	return 0;
}

SYS_INIT(initialize, APPLICATION, 70);
