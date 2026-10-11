// SPDX-License-Identifier: MIT
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "processing.h"
#include "stationary.h"

static pbio_imu_persistent_settings_t settings;
static const pbio_geometry_xyz_t zero;
static const pbio_geometry_xyz_t gravity = { .z = 9806.65f };

static void reset(void) {
    imu_processing_reset();
    imu_processing_default_settings(&settings);
    imu_processing_apply_settings(&settings);
}

static void near(float actual, float expected, float tolerance) {
    if (!isfinite(actual) || fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "got %.9g, expected %.9g +/- %.9g\n", actual, expected, tolerance);
        assert(0);
    }
}

static void poses_and_reads(void) {
    const pbio_geometry_side_t sides[] = {
        PBIO_GEOMETRY_SIDE_FRONT, PBIO_GEOMETRY_SIDE_LEFT, PBIO_GEOMETRY_SIDE_TOP,
        PBIO_GEOMETRY_SIDE_BACK, PBIO_GEOMETRY_SIDE_RIGHT, PBIO_GEOMETRY_SIDE_BOTTOM,
    };
    for (unsigned int face = 0; face < 6; face++) {
        reset();
        pbio_geometry_xyz_t g = {0}, acceleration, tilt;
        g.values[face % 3] = face < 3 ? gravity.z : -gravity.z;
        assert(imu_processing_sample(&zero, &g, 0) == PBIO_SUCCESS);
        for (unsigned int repeat = 0; repeat < 5; repeat++) {
            assert(pbio_imu_get_up_side(true) == sides[face]);
            assert(pbio_imu_get_up_side(false) == sides[face]);
            pbio_imu_get_acceleration(&acceleration, false);
            assert(!memcmp(&acceleration, &g, sizeof(g)));
        }
        pbio_imu_get_tilt_vector(&tilt);
        for (unsigned int axis = 0; axis < 3; axis++) {
            near(tilt.values[axis], g.values[axis] / gravity.z, 0.00001f);
        }
    }
    reset();
    assert(imu_processing_sample(&zero, &zero, 0) == PBIO_ERROR_AGAIN);
    assert(imu_processing_sample(&zero, &gravity, 0) == PBIO_SUCCESS);
    pbio_geometry_xyz_t bad = { .x = NAN };
    assert(imu_processing_sample(&bad, &gravity, 0.001f) == PBIO_ERROR_INVALID_ARG);
    assert(imu_processing_sample(&zero, &gravity, -0.1f) == PBIO_ERROR_INVALID_ARG);
    assert(imu_processing_sample(&zero, &gravity, INFINITY) == PBIO_ERROR_INVALID_ARG);
    pbio_geometry_xyz_t acceleration;
    pbio_imu_get_acceleration(&acceleration, false);
    assert(!memcmp(&acceleration, &gravity, sizeof(gravity)));
}

static void rotations(void) {
    reset();
    assert(imu_processing_sample(&zero, &gravity, 0) == PBIO_SUCCESS);
    pbio_geometry_xyz_t rate = { .z = 90 }, z = { .z = 1 };
    for (unsigned int i = 0; i < 8000; i++) {
        assert(imu_processing_sample(&rate, &gravity, 0.001f) == PBIO_SUCCESS);
    }
    // An eight-second turn at 90 deg/s: two complete revolutions.
    near(pbio_imu_get_heading(PBIO_IMU_HEADING_TYPE_3D), -720, 0.02f);
    near(pbio_imu_get_heading(PBIO_IMU_HEADING_TYPE_1D), -720, 0.08f);
    float angle;
    assert(pbio_imu_get_single_axis_rotation(&z, &angle, true) == PBIO_SUCCESS);
    near(angle, 720, 0.08f);
    pbio_imu_set_heading(37);
    near(pbio_imu_get_heading(PBIO_IMU_HEADING_TYPE_1D), 37, 0.0001f);
    near(pbio_imu_get_heading(PBIO_IMU_HEADING_TYPE_3D), 37, 0.0001f);
    assert(imu_processing_sample(&rate, &gravity, 0) == PBIO_SUCCESS);
    near(pbio_imu_get_heading(PBIO_IMU_HEADING_TYPE_3D), 37, 0.0001f);

    // A single small integration must be visible immediately (no one-frame lag).
    assert(imu_processing_sample(&rate, &gravity, 0.001f) == PBIO_SUCCESS);
    near(pbio_imu_get_heading(PBIO_IMU_HEADING_TYPE_3D), 36.91f, 0.0001f);

    // Rotation about a non-axis-aligned gravity vector is still pure yaw.
    reset();
    pbio_geometry_xyz_t tilted_g = { .x = 0.6f * gravity.z, .z = 0.8f * gravity.z };
    rate = (pbio_geometry_xyz_t) { .x = 54, .z = 72 };
    assert(imu_processing_sample(&zero, &tilted_g, 0) == PBIO_SUCCESS);
    float heading = pbio_imu_get_heading(PBIO_IMU_HEADING_TYPE_3D);
    for (unsigned int i = 0; i < 1000; i++) {
        assert(imu_processing_sample(&rate, &tilted_g, 0.001f) == PBIO_SUCCESS);
    }
    near(pbio_imu_get_heading(PBIO_IMU_HEADING_TYPE_3D), heading - 90, 0.005f);
    pbio_geometry_xyz_t tilt;
    pbio_imu_get_tilt_vector(&tilt);
    near(tilt.x, 0.6f, 0.0001f);
    near(tilt.y, 0, 0.0001f);
    near(tilt.z, 0.8f, 0.0001f);
}

static void calibration(void) {
    reset();
    pbio_imu_persistent_settings_t changed = settings;
    changed.flags = PBIO_IMU_SETTINGS_FLAGS_GYRO_SCALE_SET | PBIO_IMU_SETTINGS_FLAGS_GYRO_BIAS_INITIAL_SET |
        PBIO_IMU_SETTINGS_FLAGS_ACCEL_CALIBRATED;
    changed.angular_velocity_bias_start = (pbio_geometry_xyz_t) { .x = 1, .y = 2, .z = 3 };
    changed.angular_velocity_scale = (pbio_geometry_xyz_t) { .x = 350, .y = 360, .z = 370 };
    for (unsigned int i = 0; i < 3; i++) {
        changed.gravity_pos.values[i] = gravity.z + 100;
        changed.gravity_neg.values[i] = -gravity.z + 100;
    }
    assert(imu_processing_set_settings(&changed) == PBIO_SUCCESS);
    imu_processing_apply_settings(&settings);
    pbio_geometry_xyz_t rate = { .x = 351, .y = 362, .z = 373 };
    pbio_geometry_xyz_t acceleration = { .x = 100, .y = 100, .z = gravity.z + 100 };
    assert(imu_processing_sample(&rate, &acceleration, 0) == PBIO_SUCCESS);
    pbio_geometry_xyz_t result;
    pbio_imu_get_angular_velocity(&result, true);
    for (unsigned int i = 0; i < 3; i++) {
        near(result.values[i], 360, 0.0001f);
    }
    pbio_imu_get_acceleration(&result, true);
    near(result.x, 0, 0.001f);
    near(result.y, 0, 0.001f);
    near(result.z, gravity.z, 0.002f);
    pbio_imu_get_angular_velocity(&result, false);
    assert(!memcmp(&result, &rate, sizeof(rate)));

    pbio_geometry_xyz_t front = { .y = 1 }, top = { .z = 1 };
    assert(pbio_imu_set_base_orientation(&front, &top) == PBIO_SUCCESS);
    pbio_imu_get_angular_velocity(&result, false);
    // Hub front points along robot +Y; hub left therefore points along -X.
    near(result.x, -362, 0.0001f);
    near(result.y, 351, 0.0001f);
    near(result.z, 373, 0.0001f);
    pbio_geometry_xyz_t invalid = { .x = NAN };
    assert(pbio_imu_set_base_orientation(&invalid, &top) == PBIO_ERROR_INVALID_ARG);
    assert(pbio_imu_set_base_orientation(&top, &top) == PBIO_ERROR_INVALID_ARG);
    pbio_geometry_xyz_t unchanged;
    pbio_imu_get_angular_velocity(&unchanged, false);
    assert(!memcmp(&result, &unchanged, sizeof(result)));
}

static void settings_rejection(void) {
    reset();
    pbio_imu_persistent_settings_t before = settings, patch = settings;
    patch.flags = PBIO_IMU_SETTINGS_FLAGS_GYRO_SCALE_SET | PBIO_IMU_SETTINGS_FLAGS_ACCEL_STATIONARY_THRESHOLD_SET;
    patch.accel_stationary_threshold = 800;
    patch.angular_velocity_scale.x = 355;
    patch.angular_velocity_scale.y = 0;
    assert(imu_processing_set_settings(&patch) == PBIO_ERROR_INVALID_ARG);
    assert(!memcmp(&before, &settings, sizeof(settings)));
    const float invalid[] = { NAN, INFINITY, -INFINITY };
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        patch = settings;
        patch.flags = PBIO_IMU_SETTINGS_FLAGS_ACCEL_CALIBRATED;
        patch.gravity_pos.y = invalid[i];
        assert(imu_processing_set_settings(&patch) == PBIO_ERROR_INVALID_ARG);
        assert(!memcmp(&before, &settings, sizeof(settings)));
        patch = settings;
        patch.flags = PBIO_IMU_SETTINGS_FLAGS_GYRO_BIAS_INITIAL_SET;
        patch.angular_velocity_bias_start.z = invalid[i];
        assert(imu_processing_set_settings(&patch) == PBIO_ERROR_INVALID_ARG);
        assert(!memcmp(&before, &settings, sizeof(settings)));
    }
    patch = settings;
    patch.flags = PBIO_IMU_SETTINGS_FLAGS_GYRO_STATIONARY_THRESHOLD_SET;
    patch.gyro_stationary_threshold = -1;
    assert(imu_processing_set_settings(&patch) == PBIO_ERROR_INVALID_ARG);
    assert(!memcmp(&before, &settings, sizeof(settings)));
}

static void stationary(void) {
    reset();
    pbio_geometry_xyz_t average = { .x = 1, .y = -2, .z = 3 }, bias;
    uint32_t now = UINT32_MAX - 500;
    assert(!imu_processing_ready(now));
    assert(!imu_processing_stationary(&average, now));
    assert(imu_processing_ready(now + 1000));
    assert(!imu_processing_stationary(&average, now + 1000));
    assert(imu_processing_stationary(&average, now + 2000));
    assert(settings.flags & PBIO_IMU_SETTINGS_FLAGS_GYRO_BIAS_INITIAL_SET);
    assert(!imu_processing_stationary(&average, now + 3000));
    imu_processing_get_bias(&bias);
    for (unsigned int i = 0; i < 3; i++) {
        near(bias.values[i], average.values[i], 0.000001f);
    }
    assert(!imu_processing_ready(now + 603000));
    average.z = 7;
    assert(!imu_processing_stationary(&average, now + 604000));
    imu_processing_get_bias(&bias);
    near(bias.z, 7, 0.000001f);
}

static void restart_orientation(void) {
    reset();
    pbio_geometry_xyz_t bias = { .x = 1, .y = 2, .z = 3 }, measured;
    pbio_geometry_xyz_t x = { .y = 1 }, z = { .z = 1 };
    assert(pbio_imu_set_base_orientation(&x, &z) == PBIO_SUCCESS);
    imu_processing_stationary(&bias, 1000);
    pbio_geometry_xyz_t rate = { .x = 10, .y = 20, .z = 30 }, before, after;
    assert(imu_processing_sample(&rate, &gravity, 0.1f) == PBIO_SUCCESS);
    pbio_imu_get_angular_velocity(&before, true);
    pbio_imu_persistent_settings_t saved = settings;
    imu_processing_restart_orientation();
    imu_processing_get_bias(&measured);
    assert(!memcmp(&bias, &measured, sizeof(bias)));
    assert(!memcmp(&settings, &saved, sizeof(settings)));
    assert(imu_processing_ready(1100));
    assert(imu_processing_sample(&rate, &gravity, 0) == PBIO_SUCCESS);
    pbio_imu_get_angular_velocity(&after, true);
    assert(!memcmp(&before, &after, sizeof(before)));
    float rotation;
    assert(pbio_imu_get_single_axis_rotation(&z, &rotation, true) == PBIO_SUCCESS);
    near(rotation, 0, 0.000001f);
}

static void stationary_windows(void) {
    imu_stationary_t state = {0};
    int16_t data[] = { 5, -6, 7, 0, 0, 4096 };
    bool complete = false;
    for (unsigned int i = 0; i < 1000 && !complete; i++) {
        complete = imu_stationary_update(&state, data, 20, 100);
    }
    assert(complete && state.stationary);
    for (unsigned int i = 0; i < 6; i++) {
        assert(state.sum[i] / state.count == data[i]);
    }
    for (unsigned int i = 1; i < IMU_STATIONARY_WINDOW_SAMPLES; i++) {
        assert(!imu_stationary_update(&state, data, 20, 100));
        assert(state.stationary);
    }
    assert(imu_stationary_update(&state, data, 20, 100));
    data[3] += 1000;
    assert(!imu_stationary_update(&state, data, 20, 100));
    assert(!state.stationary && state.count == 0);

    // A full-range jump must not wrap to a small signed 16-bit difference.
    memset(&state, 0, sizeof(state));
    data[0] = 32760;
    complete = false;
    for (unsigned int i = 0; i < 1000 && !complete; i++) {
        complete = imu_stationary_update(&state, data, 20, 100);
    }
    assert(complete);
    data[0] = -32760;
    assert(!imu_stationary_update(&state, data, 20, 100));
    assert(!state.stationary && state.count == 0);

    memset(&state, 0, sizeof(state));
    memset(data, 0, sizeof(data));
    for (unsigned int i = 0; i < 1000; i++) {
        assert(!imu_stationary_update(&state, data, 0, 0));
    }
    assert(!state.stationary && state.count == 0);
}

int imu_processing_checks(void) {
    poses_and_reads();
    rotations();
    calibration();
    settings_rejection();
    stationary();
    stationary_windows();
    restart_orientation();
    puts("IMU processing checks passed");
    return 0;
}
