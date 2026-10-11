// SPDX-License-Identifier: MIT
// Copyright (c) 2020-2026 The Pybricks Authors

#include <string.h>
#include "stationary.h"

static void reset_window(imu_stationary_t *state) {
    state->count = 0;
    memset(state->sum, 0, sizeof(state->sum));
}

bool imu_stationary_update(imu_stationary_t *state, const int16_t data[6],
    int16_t gyro_threshold, int16_t accel_threshold) {
    // Keep the previous complete window available until its caller consumes it.
    if (state->count == IMU_STATIONARY_WINDOW_SAMPLES) {
        reset_window(state);
    }

    for (uint8_t i = 0; i < 6; i++) {
        state->slow_sum[i] += data[i];
    }
    if (++state->slow_count == IMU_STATIONARY_BASELINE_SAMPLES) {
        for (uint8_t i = 0; i < 6; i++) {
            state->slow[i] = state->slow_sum[i] / state->slow_count;
            state->slow_sum[i] = 0;
        }
        state->slow_count = 0;
    }

    // Compare with the constant start sample, not a baseline that follows motion.
    for (uint8_t i = 0; i < 6; i++) {
        int32_t difference = (int32_t)data[i] - state->start[i];
        int32_t threshold = i < 3 ? gyro_threshold : accel_threshold;
        if (difference >= threshold || difference <= -threshold) {
            state->stationary = false;
            memcpy(state->start, state->slow, sizeof(state->start));
            reset_window(state);
            return false;
        }
    }

    for (uint8_t i = 0; i < 6; i++) {
        state->sum[i] += data[i];
    }
    if (++state->count < IMU_STATIONARY_WINDOW_SAMPLES) {
        return false;
    }
    state->stationary = true;
    return true;
}
