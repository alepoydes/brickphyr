// SPDX-License-Identifier: MIT
// Copyright (c) 2020-2026 The Pybricks Authors
#ifndef _IMU_STATIONARY_H_
#define _IMU_STATIONARY_H_

#include <stdbool.h>
#include <stdint.h>

// The existing 833 Hz IMU uses a 125-sample baseline and one-second windows.
#define IMU_STATIONARY_BASELINE_SAMPLES 125
#define IMU_STATIONARY_WINDOW_SAMPLES 833

typedef struct {
    int16_t slow[6];
    int16_t start[6];
    int32_t slow_sum[6];
    uint16_t slow_count;
    uint16_t count;
    int32_t sum[6];
    bool stationary;
} imu_stationary_t;

/** Zero-initialize the state, also after missing samples or changing thresholds.
 * Samples are contiguous gyro XYZ then acceleration XYZ, before conversion to
 * physical units. Positive thresholds use the same raw units; zero disables
 * detection. Differences and sums are wider than the signed 16-bit samples.
 * Returns true for a complete window: sum/count remain readable until the next
 * call. A count of zero indicates detected motion; stationary becomes true at
 * the first complete window and false immediately on motion.
 * This detects constant signals, not absolute rest: the adapter must also gate
 * bias learning on actual motor activity as required by its platform.
 */
bool imu_stationary_update(imu_stationary_t *state, const int16_t data[6],
    int16_t gyro_threshold, int16_t accel_threshold);

#endif // _IMU_STATIONARY_H_
