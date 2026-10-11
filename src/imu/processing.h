// SPDX-License-Identifier: MIT
#ifndef BRICKPHYR_IMU_PROCESSING_H_
#define BRICKPHYR_IMU_PROCESSING_H_

#include <pbio/imu.h>

/**
 * Private boundary of Brickphyr's port of the Pybricks IMU estimator.
 * Serialize processing, settings and all pbio_imu getters in the caller.
 * There is no allocation, device access, clock lookup or storage write here.
 */
/** Reset cached samples, orientation, user frame, bias, readiness and binding. */
void imu_processing_reset(void);
/** Restart integration after a discontinuity, retaining bias, settings and axes. */
void imu_processing_restart_orientation(void);
void imu_processing_default_settings(pbio_imu_persistent_settings_t *settings);

/** Bind valid caller-owned settings, which must outlive the estimator.
 * Also restores the initial bias. Does not reset orientation or rotation. */
void imu_processing_apply_settings(pbio_imu_persistent_settings_t *settings);

/** Validate all selected fields before modifying the bound settings.
 * Bias-start is a boot value; apply_settings explicitly makes it effective now. */
pbio_error_t imu_processing_set_settings(const pbio_imu_persistent_settings_t *settings);

/** Physical hub axes: degrees/s and mm/s^2. sample_time is elapsed seconds.
 * Zero initializes/refreshes a sample without integrating unobserved time.
 * AGAIN means the first gravity vector was zero; invalid input is not applied.
 * The adapter owns handling of missing samples and validity of the estimate. */
pbio_error_t imu_processing_sample(const pbio_geometry_xyz_t *angular_velocity,
    const pbio_geometry_xyz_t *acceleration, float sample_time);

/** Supply the mean gyro rate in degrees/s for a verified stationary window.
 * Returns true if the bound settings gained their first learned initial bias.
 * The caller decides whether and when to persist it. now_ms wraps modulo 2^32. */
bool imu_processing_stationary(const pbio_geometry_xyz_t *gyro_average, uint32_t now_ms);
bool imu_processing_ready(uint32_t now_ms);
void imu_processing_get_bias(pbio_geometry_xyz_t *bias);

#endif // BRICKPHYR_IMU_PROCESSING_H_
