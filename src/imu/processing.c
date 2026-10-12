// SPDX-License-Identifier: MIT
// Copyright (c) 2022-2023 The Pybricks Authors

#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <pbio/angle.h>
#include <pbio/config.h>
#include <pbio/error.h>
#include <pbio/geometry.h>
#include <pbio/imu.h>
#include "processing.h"
#include <pbio/util.h>

#if PBIO_CONFIG_IMU

/**
 * Uncalibrated angular velocity in the hub frame.
 *
 * These are scaled from raw units to degrees per second using only the
 * datasheet/hal conversion constant, but otherwise not further adjusted.
 */
static pbio_geometry_xyz_t angular_velocity_uncalibrated;

/**
 * Estimated gyro bias value in degrees per second.
 *
 * This is a measure for the uncalibrated angular velocity above, averaged over
 * time. If specified, the value starts at the last saved user value, then
 * updates over time.
 */
static pbio_geometry_xyz_t gyro_bias;

/**
 * Calibrated angular velocity in the hub frame degrees per second.
 *
 * This takes the uncalibrated value above, subtracts the bias estimate, and
 * rescales by a user calibration factor to ensure that integrating over one
 * full rotation adds up to 360 degrees.
 */
static pbio_geometry_xyz_t angular_velocity_calibrated;

/**
 * Uncalibrated acceleration in the hub frame in mm/s^2.
 *
 * These are scaled from raw units to mm/s^2 using only the
 * datasheet/hal conversion constant, but otherwise not further adjusted.
 */
static pbio_geometry_xyz_t acceleration_uncalibrated;

/**
 * Calibrated acceleration in the hub frame mm/s^2.
 *
 * This takes the uncalibrated value above, and subtracts a constant user offset
 * and scales by a previously determined user factor to normalize to gravity magnitude.
 */
static pbio_geometry_xyz_t acceleration_calibrated; // mm/s^2, in hub frame

/**
 * 1D integrated angular velocity for each body axis.
 *
 * This is based on integrating the calibrated angular velocity over time, so
 * including its bias and adjustments to achieve 360 degrees per rotation.
 *
 * This is not used for 3D attitude estimation, but serves as a useful way to
 * estimate 1D rotations without being effected by accelerometer fusion which
 * may leads to unwanted adjustments in applications like balancing robots.
 */
static pbio_geometry_xyz_t single_axis_rotation; // deg, in hub frame

/**
 * Rotation of the hub with respect to the inertial frame, see R(q) below.
 *
 * Initialized as the identity quaternion. Updated on first gravity sample.
 */
static pbio_geometry_quaternion_t quaternion = {
    .q1 = 0.0f,
    .q2 = 0.0f,
    .q3 = 0.0f,
    .q4 = 1.0f,
};

/**
 * Flag to indicate if the quaternion has been initialized to the very first
 * gravity sample.
 */
static bool quaternion_initialized = false;

/**
 * Rotation of the hub with respect to the inertial frame.
 *
 * Does *not* use the user application frame.
 *
 * The matrix R(q) is defined such that it transforms hub body frame vectors to
 * vectors in the inertial frame as:
 *
 *    v_inertial = R(q) * v_body
 *
 * Initialized as the identity matrix. Must match initial value of quaternion.
 */
static pbio_geometry_matrix_3x3_t pbio_imu_rotation = {
    .m11 = 1.0f, .m12 = 0.0f, .m13 = 0.0f,
    .m21 = 0.0f, .m22 = 1.0f, .m23 = 0.0f,
    .m31 = 0.0f, .m32 = 0.0f, .m33 = 1.0f,
};


/**
 * The "neutral" base orientation of the hub, describing how it is mounted
 * in the robot. All getters (tilt, acceleration, rotation, etc) give results
 * relative to this base orientation:
 *
 * vector_reported = R_base * vector_in_hub_body_frame
 *
 * Default orientation is identity, hub flat.
 */
static imu_processing_frame_t default_frame = {
    .base = { .m11 = 1, .m22 = 1, .m33 = 1 },
};

/**
 * Hub calibration settings. Cannot be used until loaded.
 */
static pbio_imu_persistent_settings_t *persistent_settings = NULL;

/**
 * Standard gravity in mm/s^2.
 */
static const float standard_gravity = 9806.65f;

/**
 * Populates defaults without accessing a driver or storage.
 *
 * @param [out] settings  The defaults.
 */
void imu_processing_default_settings(pbio_imu_persistent_settings_t *settings) {
    settings->flags = 0;
    settings->gyro_stationary_threshold = 2.0f;
    settings->accel_stationary_threshold = 2500.0f;
    settings->gravity_pos.x = settings->gravity_pos.y = settings->gravity_pos.z = standard_gravity;
    settings->gravity_neg.x = settings->gravity_neg.y = settings->gravity_neg.z = -standard_gravity;
    settings->angular_velocity_bias_start.x = settings->angular_velocity_bias_start.y = settings->angular_velocity_bias_start.z = 0.0f;
    settings->angular_velocity_scale.x = settings->angular_velocity_scale.y = settings->angular_velocity_scale.z = 360.0f;
    settings->heading_correction_1d = 360.0f;
}

/**
 * Binds caller-owned settings and applies their initial gyro bias.
 *
 * @param [in]  settings  The loaded settings to apply.
 */
void imu_processing_apply_settings(pbio_imu_persistent_settings_t *settings) {
    // The caller retains the settings for the estimator's lifetime.
    persistent_settings = settings;

    // The saved angular velocity bias only sets the initial value. We still
    // update the bias continuously from stationary data.
    gyro_bias.x = settings->angular_velocity_bias_start.x;
    gyro_bias.y = settings->angular_velocity_bias_start.y;
    gyro_bias.z = settings->angular_velocity_bias_start.z;
}

/**
 * Given current orientation matrix, update the heading projection.
 *
 * This is called from the update loop so we can catch the projection jumping
 * across the 180/-180 boundary, and increment or decrement the rotation to
 * have a continuous heading.
 *
 * This is also called when the orientation frame is changed because this sets
 * the application x-axis used for the heading projection.
 */
void imu_processing_frame_update(imu_processing_frame_t *frame) {

    // Transform application x axis back into the hub frame (R_base^T * x_unit).
    pbio_geometry_xyz_t x_application = {
        .x = frame->base.m11,
        .y = frame->base.m12,
        .z = frame->base.m13
    };

    // Transform application x axis into the inertial frame via quaternion matrix.
    pbio_geometry_xyz_t x_inertial;
    pbio_geometry_vector_map(&pbio_imu_rotation, &x_application, &x_inertial);

    // Project onto the horizontal plane and use atan2 to get the angle.
    float heading_now = pbio_geometry_radians_to_degrees(atan2f(-x_inertial.y, x_inertial.x));

    // Update full rotation counter if the projection jumps across the 180/-180 boundary.
    if (heading_now < -90 && frame->projection > 90) {
        frame->rotations++;
    } else if (heading_now > 90 && frame->projection < -90) {
        frame->rotations--;
    }
    frame->projection = heading_now;
}

// Process physical samples supplied by the platform adapter.
pbio_error_t imu_processing_sample(const pbio_geometry_xyz_t *angular_velocity,
    const pbio_geometry_xyz_t *acceleration, float sample_time) {

    if (!angular_velocity || !acceleration || !isfinite(sample_time) || sample_time < 0) {
        return PBIO_ERROR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < 3; i++) {
        if (!isfinite(angular_velocity->values[i]) || !isfinite(acceleration->values[i])) {
            return PBIO_ERROR_INVALID_ARG;
        }
    }

    // Initialize quaternion from first gravity sample as a best-effort estimate.
    // From here, fusion will gradually converge the quaternion to the true value.
    if (!quaternion_initialized) {
        pbio_geometry_xyz_t g = *acceleration;
        pbio_error_t err = pbio_geometry_vector_normalize(&g, &g);
        if (err != PBIO_SUCCESS) {
            // First sample not suited, try again on next sample.
            return PBIO_ERROR_AGAIN;
        }
        pbio_geometry_quaternion_from_gravity_unit_vector(&g, &quaternion);
        quaternion_initialized = true;
        pbio_geometry_quaternion_to_rotation_matrix(&quaternion, &pbio_imu_rotation);
    }

    for (uint8_t i = 0; i < PBIO_ARRAY_SIZE(angular_velocity_calibrated.values); i++) {
        // Update angular velocity and acceleration cache so user can read them.
        angular_velocity_uncalibrated.values[i] = angular_velocity->values[i];
        acceleration_uncalibrated.values[i] = acceleration->values[i];

        // Once settings loaded, maintain calibrated cached values.
        if (persistent_settings) {
            float acceleration_offset = (persistent_settings->gravity_pos.values[i] + persistent_settings->gravity_neg.values[i]) / 2;
            float acceleration_scale = (persistent_settings->gravity_pos.values[i] - persistent_settings->gravity_neg.values[i]) / 2;
            acceleration_calibrated.values[i] = (acceleration_uncalibrated.values[i] - acceleration_offset) * standard_gravity / acceleration_scale;
            angular_velocity_calibrated.values[i] = (angular_velocity_uncalibrated.values[i] - gyro_bias.values[i]) * 360.0f / persistent_settings->angular_velocity_scale.values[i];
        } else {
            acceleration_calibrated.values[i] = acceleration_uncalibrated.values[i];
            angular_velocity_calibrated.values[i] = angular_velocity_uncalibrated.values[i];
        }

        // Update "heading" on all axes. This is not useful for 3D attitude
        // estimation, but it allows the user to get a 1D heading even with
        // the hub mounted at an arbitrary orientation. Such a 1D heading
        // is numerically more accurate, which is useful in drive base
        // applications so long as the vehicle drives on a flat surface.
        single_axis_rotation.values[i] += angular_velocity_calibrated.values[i] * sample_time;
    }

    // Estimate for gravity vector based on orientation estimate.
    pbio_geometry_xyz_t s = {
        .x = pbio_imu_rotation.m31,
        .y = pbio_imu_rotation.m32,
        .z = pbio_imu_rotation.m33,
    };

    // We would like to adjust the attitude such that the gravity estimate
    // converges to the gravity value in the stationary case. If we subtract
    // both vectors we get the required direction of changes. This can be
    // thought of as a virtual spring between both vectors. This produces a
    // moment about the origin, which ultimately simplies to the following,
    // which we inject to the attitude integration.
    pbio_geometry_xyz_t correction;
    pbio_geometry_vector_cross_product(&s, &acceleration_calibrated, &correction);

    // Qualitative measures for how far the current state is from being stationary.
    float accl_stationary_error = pbio_geometry_absf(pbio_geometry_vector_norm(&acceleration_calibrated) - standard_gravity);
    float gyro_stationary_error = pbio_geometry_absf(pbio_geometry_vector_norm(&angular_velocity_calibrated));

    // Cut off value below which value is considered stationary enough for fusion.
    const float gyro_stationary_min = 10;
    const float accl_stationary_min = 150;

    // Measure for being statinonary ranging from 0 (moving) to 1 (moving less than above thresholds).
    float stationary_measure = accl_stationary_min / pbio_geometry_maxf(accl_stationary_error, accl_stationary_min) *
        gyro_stationary_min / pbio_geometry_maxf(gyro_stationary_error, gyro_stationary_min);

    // The virtual moment would produce motion in that direction, so we can
    // simulate that effect by injecting it into the attitude integration, the
    // strength of which is based on the stationary measure. It is scaled down
    // by the gravity amount since one of the two vectors to produce this has
    // units of gravity. Hence if the hub is stationary (measure = 1), and the
    // error is 90 degrees (which is unlikely), the correction is at
    // most 200 deg/s, but usually much less.
    float fusion = -stationary_measure / standard_gravity * 200;
    pbio_geometry_xyz_t adjusted_angular_velocity;
    adjusted_angular_velocity.x = angular_velocity_calibrated.x + correction.x * fusion;
    adjusted_angular_velocity.y = angular_velocity_calibrated.y + correction.y * fusion;
    adjusted_angular_velocity.z = angular_velocity_calibrated.z + correction.z * fusion;

    // Update 3D attitude, basic forward integration.
    pbio_geometry_quaternion_t dq;
    pbio_geometry_quaternion_get_rate_of_change(&quaternion, &adjusted_angular_velocity, &dq);
    for (uint8_t i = 0; i < PBIO_ARRAY_SIZE(dq.values); i++) {
        quaternion.values[i] += dq.values[i] * sample_time;
    }
    pbio_geometry_quaternion_normalize(&quaternion);

    // All outputs describe this sample, not the previous integration step.
    pbio_geometry_quaternion_to_rotation_matrix(&quaternion, &pbio_imu_rotation);
    imu_processing_frame_update(&default_frame);
    return PBIO_SUCCESS;
}

// This counter is a measure for calibration accuracy, roughly equivalent
// to the accumulative number of seconds it has been stationary in total.
static uint32_t stationary_counter = 0;
static uint32_t stationary_time_last;

/*
 * Tests if the imu is ready for use in a user program.
 *
 * @return    True if it has been stationary at least once in the last 10 minutes.
*/
bool imu_processing_ready(uint32_t now_ms) {
    return stationary_counter > 0 && now_ms - stationary_time_last < 10 * 60 * 1000;
}

// The adapter supplies a gyro average only after validating a stationary window.
bool imu_processing_stationary(const pbio_geometry_xyz_t *gyro_average, uint32_t now_ms) {

    // If the IMU calibration hasn't been updated in a long time, reset the
    // stationary counter so that the calibration values get a large weight.
    if (!imu_processing_ready(now_ms)) {
        stationary_counter = 0;
    }

    stationary_time_last = now_ms;
    if (stationary_counter < 20) {
        stationary_counter++;
    }

    // The relative weight of the new data in order to build a long term
    // average of the data without maintaining a data buffer.
    float weight = stationary_counter >= 20 ? 0.05f : 1.0f / stationary_counter;

    for (uint8_t i = 0; i < PBIO_ARRAY_SIZE(gyro_bias.values); i++) {
        // Average gyro rate while stationary, indicating current bias.
        float average_now = gyro_average->values[i];

        // Update bias at decreasing rate.
        gyro_bias.values[i] = gyro_bias.values[i] * (1.0f - weight) + weight * average_now;
    }

    // Report a newly learned initial bias to the adapter; it owns saving.
    // This ensures a better starting point for the next boot. We do this only
    // once to avoid unnecessary writes on every shutdown. It can be further
    // refined with a calibration routine performed by the user.
    if (persistent_settings && !(persistent_settings->flags & PBIO_IMU_SETTINGS_FLAGS_GYRO_BIAS_INITIAL_SET) && stationary_counter > 2) {
        persistent_settings->angular_velocity_bias_start = gyro_bias;
        persistent_settings->flags |= PBIO_IMU_SETTINGS_FLAGS_GYRO_BIAS_INITIAL_SET;
        return true;
    }
    return false;
}

/**
 * Sets the hub base orientation.
 *
 * @param [in]  front_side_axis  Which way the hub front side points when it is
 *                               in the base orientation.
 * @param [in]  top_side_axis    Which way the hub top side points when it is
 *                               in the base orientation.
 * @return                       ::PBIO_SUCCESS on success, ::PBIO_ERROR_INVALID_ARG for incorrect axis values.
 */
pbio_error_t pbio_imu_set_base_orientation(pbio_geometry_xyz_t *front_side_axis, pbio_geometry_xyz_t *top_side_axis) {

    pbio_error_t err = pbio_geometry_map_from_base_axes(front_side_axis, top_side_axis, &default_frame.base);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Need to update heading projection since the application axes were changed.
    imu_processing_frame_update(&default_frame);

    // Reset offsets such that the new frame starts with zero heading.
    pbio_imu_set_heading(0.0f);
    return PBIO_SUCCESS;
}

/**
 * Tests if the acceleration value is within a reasonable range for a stationary hub.
 *
 * @param [in]  value  The acceleration value to test.
 * @return             True if the value is within 10% off from standard gravity.
 */
static bool pbio_imu_stationary_acceleration_out_of_range(float value, bool expect_positive) {
    const float expected_value = expect_positive ? standard_gravity : -standard_gravity;
    const float absolute_error = value > expected_value ? value - expected_value : expected_value - value;
    return !isfinite(value) || absolute_error > standard_gravity / 10;
}

/**
 * Tests if a value is close to 360 degrees.
 *
 * @param [in]  value  The value to test.
 * @return             True if the value is within +/-15 degrees of 360, false otherwise.
 */
static bool pbio_imu_setting_close_to_360(float value) {
    return pbio_geometry_absf(value - 360.0f) < 15.0f;
}

/**
 * Sets the IMU settings. This includes the thresholds that define when the hub
 * is stationary. The adapter uses these thresholds to select calibration
 * windows. Also includes the hub-specific
 * correction value to get a more accurate heading value.
 *
 * Note: the flags in this setter are not used to reset the flags value in the
 * persistent settings but to select which settings are being updated here.
 *
 * @param [in]  new_settings        Incomplete set of new settings to apply according to the flags..
 * @returns ::PBIO_ERROR_INVALID_ARG if a value is out of range, otherwise ::PBIO_SUCCESS.
 */
pbio_error_t imu_processing_set_settings(const pbio_imu_persistent_settings_t *new_settings) {

    // Settings must be bound before they can be read or changed.
    if (!persistent_settings) {
        return PBIO_ERROR_FAILED;
    }

    const uint32_t supported = PBIO_IMU_SETTINGS_FLAGS_ACCEL_STATIONARY_THRESHOLD_SET |
        PBIO_IMU_SETTINGS_FLAGS_GYRO_STATIONARY_THRESHOLD_SET |
        PBIO_IMU_SETTINGS_FLAGS_GYRO_BIAS_INITIAL_SET | PBIO_IMU_SETTINGS_FLAGS_GYRO_SCALE_SET |
        PBIO_IMU_SETTINGS_FLAGS_ACCEL_CALIBRATED | PBIO_IMU_SETTINGS_FLAGS_HEADING_CORRECTION_1D_SET;
    if (!new_settings || (new_settings->flags & ~supported)) {
        return PBIO_ERROR_INVALID_ARG;
    }
    // Validate a candidate before mutating any live value.
    pbio_imu_persistent_settings_t candidate = *persistent_settings;

    if (new_settings->flags & PBIO_IMU_SETTINGS_FLAGS_ACCEL_STATIONARY_THRESHOLD_SET) {
        if (!isfinite(new_settings->accel_stationary_threshold) || new_settings->accel_stationary_threshold < 0) {
            return PBIO_ERROR_INVALID_ARG;
        }
        candidate.accel_stationary_threshold = new_settings->accel_stationary_threshold;
    }

    if (new_settings->flags & PBIO_IMU_SETTINGS_FLAGS_GYRO_STATIONARY_THRESHOLD_SET) {
        if (!isfinite(new_settings->gyro_stationary_threshold) || new_settings->gyro_stationary_threshold < 0) {
            return PBIO_ERROR_INVALID_ARG;
        }
        candidate.gyro_stationary_threshold = new_settings->gyro_stationary_threshold;
    }

    for (uint8_t i = 0; i < 3; i++) {
        if (new_settings->flags & PBIO_IMU_SETTINGS_FLAGS_GYRO_BIAS_INITIAL_SET) {
            if (!isfinite(new_settings->angular_velocity_bias_start.values[i])) {
                return PBIO_ERROR_INVALID_ARG;
            }
            candidate.angular_velocity_bias_start.values[i] = new_settings->angular_velocity_bias_start.values[i];
        }

        if (new_settings->flags & PBIO_IMU_SETTINGS_FLAGS_GYRO_SCALE_SET) {
            if (!pbio_imu_setting_close_to_360(new_settings->angular_velocity_scale.values[i])) {
                return PBIO_ERROR_INVALID_ARG;
            }
            candidate.angular_velocity_scale.values[i] = new_settings->angular_velocity_scale.values[i];
        }
    }

    if (new_settings->flags & PBIO_IMU_SETTINGS_FLAGS_ACCEL_CALIBRATED) {
        if (pbio_imu_stationary_acceleration_out_of_range(new_settings->gravity_pos.x, true) ||
            pbio_imu_stationary_acceleration_out_of_range(new_settings->gravity_neg.x, false) ||
            pbio_imu_stationary_acceleration_out_of_range(new_settings->gravity_pos.y, true) ||
            pbio_imu_stationary_acceleration_out_of_range(new_settings->gravity_neg.y, false) ||
            pbio_imu_stationary_acceleration_out_of_range(new_settings->gravity_pos.z, true) ||
            pbio_imu_stationary_acceleration_out_of_range(new_settings->gravity_neg.z, false)) {
            return PBIO_ERROR_INVALID_ARG;
        }
        candidate.gravity_pos = new_settings->gravity_pos;
        candidate.gravity_neg = new_settings->gravity_neg;
    }

    if (new_settings->flags & PBIO_IMU_SETTINGS_FLAGS_HEADING_CORRECTION_1D_SET) {
        if (!pbio_imu_setting_close_to_360(new_settings->heading_correction_1d)) {
            return PBIO_ERROR_INVALID_ARG;
        }
        candidate.heading_correction_1d = new_settings->heading_correction_1d;
    }

    candidate.flags |= new_settings->flags;
    *persistent_settings = candidate;

    return PBIO_SUCCESS;
}

/**
 * Gets the IMU settings
 *
 * @param [out]  settings        Complete set of new settings.
 * @returns                      ::PBIO_ERROR_FAILED if settings not available, otherwise ::PBIO_SUCCESS.
 */
pbio_error_t pbio_imu_get_settings(pbio_imu_persistent_settings_t **settings) {
    // Settings must be bound before they can be read or changed.
    if (!persistent_settings) {
        return PBIO_ERROR_FAILED;
    }
    *settings = persistent_settings;
    return PBIO_SUCCESS;
}

/**
 * Gets the cached IMU angular velocity in deg/s, compensated for gyro bias.
 *
 * @param [out] values      The angular velocity vector.
 * @param [in]  calibrated  Whether to get calibrated or uncalibrated data.
 */
void pbio_imu_get_angular_velocity(pbio_geometry_xyz_t *values, bool calibrated) {
    pbio_geometry_xyz_t *angular_velocity = calibrated ? &angular_velocity_calibrated : &angular_velocity_uncalibrated;
    pbio_geometry_vector_map(&default_frame.base, angular_velocity, values);
}

/**
 * Gets the cached IMU acceleration in mm/s^2.
 *
 * @param [in]  calibrated  Whether to use calibrated or uncalibrated data.
 *
 * @param [out] values      The acceleration vector.
 */
void pbio_imu_get_acceleration(pbio_geometry_xyz_t *values, bool calibrated) {
    pbio_geometry_xyz_t *acceleration = calibrated ? &acceleration_calibrated : &acceleration_uncalibrated;
    pbio_geometry_vector_map(&default_frame.base, acceleration, values);
}

/**
 * Gets the tilt-based vector that is parallel to the acceleration measurement
 * if we were stationary.
 *
 * @param [out] values      The acceleration vector.
 */
void pbio_imu_get_tilt_vector(pbio_geometry_xyz_t *values) {
    pbio_geometry_xyz_t direction = {
        .x = pbio_imu_rotation.m31,
        .y = pbio_imu_rotation.m32,
        .z = pbio_imu_rotation.m33,
    };
    pbio_geometry_vector_map(&default_frame.base, &direction, values);
}

/* Per-axis integrated rotations in the physical hub frame, in degrees. */
void imu_processing_get_rotation(pbio_geometry_xyz_t *rotation, bool calibrated) {

    // Local copy so we can change it in-place.
    *rotation = single_axis_rotation;
    if (!calibrated && persistent_settings) {
        // In this context, calibrated means that the angular velocity values
        // were scaled by the user calibration factors before integrating. This
        // is done within the update loop since we need it for 3D integration.
        // Since this method only returns the separate 1D rotations, we can
        // undo this scaling here to get the "uncalibrated" values.
        for (uint8_t i = 0; i < PBIO_ARRAY_SIZE(rotation->values); i++) {
            rotation->values[i] *= persistent_settings->angular_velocity_scale.values[i] / 360.0f;
        }
    }
}

/** Project integrated rotations into the robot frame. Meaningful for 1D turns. */
pbio_error_t pbio_imu_get_single_axis_rotation(pbio_geometry_xyz_t *axis, float *angle, bool calibrated) {
    pbio_geometry_xyz_t axis_rotation;
    imu_processing_get_rotation(&axis_rotation, calibrated);
    // Transform the single axis rotations to the robot frame.
    pbio_geometry_xyz_t rotation_user;
    pbio_geometry_vector_map(&default_frame.base, &axis_rotation, &rotation_user);

    // Get the requested scalar rotation along the given axis.
    return pbio_geometry_vector_project(axis, &rotation_user, angle);
}

/**
 * Gets which side of a hub points upwards.
 *
 * @param [in]  calibrated  Whether to use calibrated/fused (true) or raw data (false).
 *
 * @return                  Which side is up.
 */
pbio_geometry_side_t pbio_imu_get_up_side(bool calibrated) {
    // Up is which side of a unit box intersects the +Z vector first.
    // So read +Z vector of the inertial frame, in the body frame.
    // This is either the raw acceleration or the third row of the fused
    // rotation matrix.

    pbio_geometry_xyz_t vector = acceleration_uncalibrated;
    if (calibrated) {
        // This is similar to pbio_imu_get_tilt_vector, but this should stay
        // in the hub frame rather than projected into user frame.
        vector.x = pbio_imu_rotation.m31;
        vector.y = pbio_imu_rotation.m32;
        vector.z = pbio_imu_rotation.m33;
    }
    return pbio_geometry_side_from_vector(&vector);
}


/**
 * Reads the estimated IMU heading in degrees, accounting for user offset and
 * user-specified heading correction scaling constant.
 *
 * Heading is defined as clockwise positive.
 *
 * @param [in]  type        The type of heading to get.
 *
 * @return                  Heading angle in the base frame.
 */
float imu_processing_frame_heading(imu_processing_frame_t *frame, pbio_imu_heading_type_t type) {

    float correction = (persistent_settings && (persistent_settings->flags & PBIO_IMU_SETTINGS_FLAGS_HEADING_CORRECTION_1D_SET)) ?
        // If set, adjust by the user-specified scaling constant.
        (360.0f / persistent_settings->heading_correction_1d):
        // No (additional) correction.
        1.0f;

    // 3D. Mapping into user frame is already accounted for in the projection.
    if (type == PBIO_IMU_HEADING_TYPE_3D) {
        return (frame->rotations * 360.0f + frame->projection) * correction - frame->offset_3d;
    }

    // 1D. Map the per-axis integrated rotation to the user frame, then take
    // the negative z component as the heading for positive-clockwise convention.
    pbio_geometry_xyz_t heading_mapped;
    pbio_geometry_vector_map(&frame->base, &single_axis_rotation, &heading_mapped);

    return -heading_mapped.z * correction - frame->offset_1d;
}

float pbio_imu_get_heading(pbio_imu_heading_type_t type) {
    return imu_processing_frame_heading(&default_frame, type);
}

/**
 * Sets the IMU heading.
 *
 * This only adjusts the user offset without resetting anything in the
 * algorithm, so this can be called at any time.
 *
 * @param [in] desired_heading  The desired heading value.
 */
void imu_processing_frame_set_heading(imu_processing_frame_t *frame, float angle) {
    frame->rotations = 0;
    frame->offset_1d = frame->offset_3d = 0;
    frame->offset_1d = imu_processing_frame_heading(frame, PBIO_IMU_HEADING_TYPE_1D) - angle;
    frame->offset_3d = imu_processing_frame_heading(frame, PBIO_IMU_HEADING_TYPE_3D) - angle;
}

void pbio_imu_set_heading(float desired_heading) {
    imu_processing_frame_set_heading(&default_frame, desired_heading);
}

void imu_processing_frame_restart(imu_processing_frame_t *frame) {
    *frame = (imu_processing_frame_t) { .base = frame->base };
}

/**
 * Gets the estimated IMU heading in control units through a given scale.
 *
 * This is mainly used to convert the heading to the right format for a
 * drivebase, which measures heading as the half the difference of the two
 * motor positions in millidegrees.
 *
 * Heading is defined as clockwise positive.
 *
 * @param [in]   type                  Heading type to get.
 * @param [out]  heading               The heading angle in control units.
 * @param [out]  heading_rate          The heading rate in control units.
 * @param [in]   ctl_steps_per_degree  The number of control steps per heading degree.
 */
void pbio_imu_get_heading_scaled(pbio_imu_heading_type_t type, pbio_angle_t *heading, int32_t *heading_rate, int32_t ctl_steps_per_degree) {

    // Heading in degrees of the robot.
    float heading_degrees = pbio_imu_get_heading(type);

    // Number of whole rotations in control units (in terms of wheels, not robot).
    heading->rotations = (int32_t)(heading_degrees / (360000.0f / ctl_steps_per_degree));

    // The truncated part represents everything else. NB: The scaling factor
    // is a float here to ensure we don't lose precision while scaling.
    float truncated = heading_degrees - heading->rotations * (360000.0f / ctl_steps_per_degree);
    heading->millidegrees = (int32_t)(truncated * ctl_steps_per_degree);

    // The heading rate can be obtained by a simple scale because it always fits.
    pbio_geometry_xyz_t angular_rate;
    pbio_imu_get_angular_velocity(&angular_rate, true);
    *heading_rate = (int32_t)(-angular_rate.z * ctl_steps_per_degree);
}

/**
 * Reads the current rotation matrix.
 *
 * @param [out] rotation      The rotation matrix
 */
void pbio_orientation_imu_get_orientation(pbio_geometry_matrix_3x3_t *rotation) {
    *rotation = pbio_imu_rotation;
}

void imu_processing_restart_orientation(void) {
    single_axis_rotation = (pbio_geometry_xyz_t) {0};
    quaternion = (pbio_geometry_quaternion_t) { .q4 = 1 };
    quaternion_initialized = false;
    pbio_imu_rotation = (pbio_geometry_matrix_3x3_t) { .m11 = 1, .m22 = 1, .m33 = 1 };
    imu_processing_frame_restart(&default_frame);
}

void imu_processing_reset(void) {
    imu_processing_restart_orientation();
    angular_velocity_uncalibrated = (pbio_geometry_xyz_t) {0};
    angular_velocity_calibrated = (pbio_geometry_xyz_t) {0};
    acceleration_uncalibrated = (pbio_geometry_xyz_t) {0};
    acceleration_calibrated = (pbio_geometry_xyz_t) {0};
    gyro_bias = (pbio_geometry_xyz_t) {0};
    default_frame.base = pbio_imu_rotation;
    stationary_counter = stationary_time_last = 0;
    persistent_settings = NULL;
}

void imu_processing_get_bias(pbio_geometry_xyz_t *bias) {
    *bias = gyro_bias;
}

#endif // PBIO_CONFIG_IMU
