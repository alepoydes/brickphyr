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
/* One protected native controller can wait; readers do not consume this signal.
 * Publication owns no pointer or resource from the waiting application. */
static K_SEM_DEFINE(sample_changed, 0, 1);
static struct bp_imu_state current = {.error = -EINPROGRESS, .raw.error = -EINPROGRESS};
static imu_processing_frame_t application_frame = {.base = {.m11 = 1, .m22 = 1, .m33 = 1}};
static struct bp_imu_application_state frame_published = {
	.physical = {.error = -EINPROGRESS, .raw.error = -EINPROGRESS},
	.base = {1, 0, 0, 0, 1, 0, 0, 0, 1},
};
/* Commands are copied before taking the short lock. Only a trusted worker may
 * hold processing_lock: aborting the requesting application cannot strand it. */
static struct {
	pbio_geometry_matrix_3x3_t base;
	uint32_t flags;
	float heading;
	uint32_t settings_mask;
	struct bp_imu_settings settings;
} frame_command;
static bool frame_releasing;
static pbio_imu_persistent_settings_t calibration;
static imu_stationary_t stationary;
static bool restart = true;
static int64_t last_us;
static uint32_t last_sequence;
static uint32_t motor_token;

enum setting { GYRO_THRESHOLD = BP_IMU_GYRO_THRESHOLD, ACCEL_THRESHOLD = BP_IMU_ACCEL_THRESHOLD,
	BIAS = BP_IMU_BIAS, GYRO_SCALE = BP_IMU_GYRO_SCALE,
	ACCEL_CALIBRATION = BP_IMU_ACCEL_CALIBRATION, HEADING = BP_IMU_HEADING_CORRECTION };
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
BUILD_ASSERT(ARRAY_SIZE(imu_config) == BP_IMU_SETTING_COUNT);
static int apply_settings(uint32_t mask, const struct bp_imu_settings *settings);

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
	pbio_geometry_xyz_t rotation;
	imu_processing_get_rotation(&rotation, false);
	float heading_1d = imu_processing_frame_heading(&application_frame, PBIO_IMU_HEADING_TYPE_1D);
	float heading_3d = imu_processing_frame_heading(&application_frame, PBIO_IMU_HEADING_TYPE_3D);
	k_spinlock_key_t key = k_spin_lock(&snapshot_lock);
	frame_published.physical = current;
	memcpy(frame_published.base, application_frame.base.values, sizeof(frame_published.base));
	memcpy(frame_published.rotation_uncalibrated, rotation.values, sizeof(rotation.values));
	frame_published.heading_1d = heading_1d;
	frame_published.heading_3d = heading_3d;
	memcpy(frame_published.settings.values, selected, sizeof(selected));
	k_spin_unlock(&snapshot_lock, key);
	k_sem_give(&sample_changed);
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
		imu_processing_frame_restart(&application_frame);
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
		imu_processing_frame_update(&application_frame);
		pbio_imu_get_acceleration(&calibrated, true);
		for (size_t i = 0; i < 3; i++) {
			current.acceleration[i] = lroundf(calibrated.values[i] * 1000);
		}
		pbio_imu_get_angular_velocity(&calibrated, true);
		imu_processing_get_bias(&bias);
		for (size_t i = 0; i < 3; i++) {
			current.angular_velocity[i] = lroundf(calibrated.values[i] * 1000);
			current.bias[i] = lroundf(bias.values[i] * 1000);
		}
		pbio_geometry_xyz_t rotation;
		imu_processing_get_rotation(&rotation, true);
		memcpy(current.rotation, rotation.values, sizeof(current.rotation));
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

static void check_age(struct bp_imu_state *state)
{
	int64_t reference = state->raw.sample_us ? state->raw.sample_us : state->raw.read_us;
	if ((!state->error || state->error == -EAGAIN) && reference &&
	    k_cyc_to_us_floor64(k_cycle_get_64()) - reference > STALE_US) {
		state->error = -ETIMEDOUT;
		state->stationary = state->ready = false;
	}
}

int hub_imu_get_state(struct bp_imu_state *state)
{
	if (!state) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&snapshot_lock);
	*state = frame_published.physical;
	k_spin_unlock(&snapshot_lock, key);
	check_age(state);
	return 0;
}

int hub_imu_wait_state(uint32_t sequence, bool previous, uint32_t timeout_ms,
	struct bp_imu_state *state)
{
	if (!state || !timeout_ms || timeout_ms > INT32_MAX) return -EINVAL;
	int64_t deadline = k_uptime_ticks() + k_ms_to_ticks_ceil64(timeout_ms);
	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&snapshot_lock);
		*state = frame_published.physical;
		k_spin_unlock(&snapshot_lock, key);
		/* Startup has no observation yet. Other processing/acquisition errors
		 * are returned as published, before an age check can obscure them. */
		bool starting = !state->raw.sample_us &&
			(state->error == -EINPROGRESS || state->error == -EAGAIN);
		if (state->error && !starting) return state->error;
		if (!state->error && (!previous || state->raw.sequence != sequence)) {
			check_age(state);
			if (!state->error) return 0;
		}
		/* A stale successful sample is not a new step. Keep one absolute
		 * deadline across stale signals/configuration-only publications. A
		 * publish between the copy and take leaves a token, so no wake is lost. */
		if (k_sem_take(&sample_changed, K_TIMEOUT_ABS_TICKS(deadline))) {
			state->error = -ETIMEDOUT;
			return -ETIMEDOUT;
		}
	}
}

int hub_imu_get_application(struct bp_imu_application_state *state)
{
	if (!state) return -EINVAL;
	k_spinlock_key_t key = k_spin_lock(&snapshot_lock);
	*state = frame_published;
	k_spin_unlock(&snapshot_lock, key);
	check_age(&state->physical);
	return 0;
}

static void apply_frame(struct k_work *work)
{
	k_mutex_lock(&processing_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&snapshot_lock);
	bool pending = frame_published.pending;
	k_spin_unlock(&snapshot_lock, key);
	if (!pending) {
		k_mutex_unlock(&processing_lock);
		return;
	}
	/* pending excludes new requests until this worker publishes completion. */
	struct bp_imu_state state = current;
	check_age(&state);
	int err = frame_command.flags & BP_IMU_FRAME_HEADING ? state.error : 0;
	if (frame_command.flags == BP_IMU_CALIBRATION) {
		err = apply_settings(frame_command.settings_mask, &frame_command.settings);
	} else if (!err) {
		if (frame_command.flags & BP_IMU_FRAME_AXES) {
			application_frame.base = frame_command.base;
			imu_processing_frame_restart(&application_frame);
			imu_processing_frame_update(&application_frame);
		}
		imu_processing_frame_set_heading(&application_frame, frame_command.heading);
	}
	publish();
	key = k_spin_lock(&snapshot_lock);
	frame_published.command_error = err;
	frame_published.pending = false;
	k_spin_unlock(&snapshot_lock, key);
	k_mutex_unlock(&processing_lock);
}
static K_WORK_DEFINE(frame_work, apply_frame);

int hub_imu_try_heading(float heading)
{
	if (!isfinite(heading)) return -EINVAL;
	if (k_mutex_lock(&processing_lock, K_NO_WAIT)) return -EAGAIN;
	k_spinlock_key_t key = k_spin_lock(&snapshot_lock);
	bool busy = frame_published.pending || frame_releasing;
	k_spin_unlock(&snapshot_lock, key);
	struct bp_imu_state state = current;
	check_age(&state);
	int err = busy ? -EAGAIN : state.error;
	if (!err) {
		imu_processing_frame_set_heading(&application_frame, heading);
		publish();
	}
	k_mutex_unlock(&processing_lock);
	return err;
}

int hub_imu_configure(const struct bp_imu_command *command)
{
	if (!command) return -EINVAL;
	struct bp_imu_command value = *command;
	if (!value.flags || (value.flags & ~(BP_IMU_FRAME_AXES | BP_IMU_FRAME_HEADING | BP_IMU_CALIBRATION)) ||
	    ((value.flags & BP_IMU_CALIBRATION) && (value.flags != BP_IMU_CALIBRATION ||
	      !value.settings_mask || value.settings_mask >> BP_IMU_SETTING_COUNT)) ||
	    ((value.flags & BP_IMU_FRAME_HEADING) && !isfinite(value.heading))) return -EINVAL;
	pbio_geometry_matrix_3x3_t base = {0};
	if (value.flags & BP_IMU_FRAME_AXES) {
		pbio_geometry_xyz_t front, top;
		memcpy(front.values, value.front, sizeof(front.values));
		memcpy(top.values, value.top, sizeof(top.values));
		if (pbio_geometry_map_from_base_axes(&front, &top, &base)) return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&snapshot_lock);
	int err = -EBUSY;
	if (!frame_published.pending && !frame_releasing) {
		frame_command.base = base;
		frame_command.flags = value.flags;
		frame_command.heading = value.flags & BP_IMU_FRAME_HEADING ? value.heading : 0;
		frame_command.settings_mask = value.settings_mask;
		frame_command.settings = value.settings;
		/* Submit while locked so release cannot miss a just-admitted command. */
		err = k_work_submit(&frame_work);
		if (err >= 0) {
			frame_published.pending = true;
			err = 0;
		}
	}
	k_spin_unlock(&snapshot_lock, key);
	return err;
}

void hub_imu_release(void)
{
	k_spinlock_key_t key = k_spin_lock(&snapshot_lock);
	frame_releasing = true;
	k_spin_unlock(&snapshot_lock, key);
	/* A queued worker owns no caller memory. Clear its request under the same
	 * mutex instead of waiting on the system queue while lifecycle is held. */
	k_mutex_lock(&processing_lock, K_FOREVER);
	application_frame = (imu_processing_frame_t){.base = {.m11 = 1, .m22 = 1, .m33 = 1}};
	imu_processing_frame_update(&application_frame);
	imu_processing_frame_set_heading(&application_frame, 0);
	publish();
	key = k_spin_lock(&snapshot_lock);
	frame_published.pending = false;
	frame_published.command_error = 0;
	frame_releasing = false;
	k_spin_unlock(&snapshot_lock, key);
	k_mutex_unlock(&processing_lock);
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

/* processing_lock is held. Build one PBIO patch, validate it completely, then
 * update the selected integers and establish a new orientation epoch. */
static int apply_settings(uint32_t mask, const struct bp_imu_settings *settings)
{
	pbio_imu_persistent_settings_t patch = {0};
	for (size_t index = 0; index < ARRAY_SIZE(imu_config); index++) {
		if (!(mask & BIT(index))) continue;
		float physical[6];
		for (size_t i = 0; i < imu_config[index].count; i++) {
			physical[i] = settings->values[index][i] / 1000.0f;
			if (index == BIAS && fabsf(physical[i]) >
			    -lsm6ds3tr_c_from_fs2000dps_to_mdps(INT16_MIN) / 1000.0f) return -EINVAL;
		}
		switch (index) {
		case GYRO_THRESHOLD:
			patch.flags |= PBIO_IMU_SETTINGS_FLAGS_GYRO_STATIONARY_THRESHOLD_SET;
			patch.gyro_stationary_threshold = physical[0];
			break;
		case ACCEL_THRESHOLD:
			patch.flags |= PBIO_IMU_SETTINGS_FLAGS_ACCEL_STATIONARY_THRESHOLD_SET;
			patch.accel_stationary_threshold = physical[0];
			break;
		case BIAS:
			patch.flags |= PBIO_IMU_SETTINGS_FLAGS_GYRO_BIAS_INITIAL_SET;
			memcpy(patch.angular_velocity_bias_start.values, physical, 3 * sizeof(float));
			break;
		case GYRO_SCALE:
			patch.flags |= PBIO_IMU_SETTINGS_FLAGS_GYRO_SCALE_SET;
			memcpy(patch.angular_velocity_scale.values, physical, 3 * sizeof(float));
			break;
		case ACCEL_CALIBRATION:
			patch.flags |= PBIO_IMU_SETTINGS_FLAGS_ACCEL_CALIBRATED;
			memcpy(patch.gravity_pos.values, physical, 3 * sizeof(float));
			memcpy(patch.gravity_neg.values, physical + 3, 3 * sizeof(float));
			break;
		case HEADING:
			patch.flags |= PBIO_IMU_SETTINGS_FLAGS_HEADING_CORRECTION_1D_SET;
			patch.heading_correction_1d = physical[0];
			break;
		}
	}
	if (imu_processing_set_settings(&patch)) return -EINVAL;
	for (size_t index = 0; index < ARRAY_SIZE(imu_config); index++) {
		if (mask & BIT(index)) {
			memcpy(selected[index], settings->values[index], imu_config[index].count * sizeof(int32_t));
		}
	}
	if (mask & BIT(BIAS)) imu_processing_apply_settings(&calibration);
	restart = true;
	current.error = -EAGAIN;
	current.ready = current.stationary = false;
	return 0;
}

static int setting_set(const char *name, size_t length, settings_read_cb read, void *context)
{
	int index = config_index(imu_config, ARRAY_SIZE(imu_config), name);
	if (index < 0 || length != imu_config[index].count * sizeof(uint32_t)) {
		return index < 0 ? index : -EINVAL;
	}
	uint8_t bytes[CONFIG_VALUE_BYTES];
	ssize_t received = read(context, bytes, length);
	if (received != length) return received < 0 ? received : -EINVAL;
	struct bp_imu_settings settings = {0};
	for (size_t i = 0; i < imu_config[index].count; i++) {
		settings.values[index][i] = (int32_t)sys_get_le32(bytes + i * sizeof(uint32_t));
	}
	k_mutex_lock(&processing_lock, K_FOREVER);
	int err = apply_settings(BIT(index), &settings);
	if (!err) publish();
	k_mutex_unlock(&processing_lock);
	return err;
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
	publish();
	return 0;
}

SYS_INIT(initialize, APPLICATION, 70);
