// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <brickphyr_imu.h>

/* Acquisition-worker boundary; data is the unflipped sensor gyro/accel frame.
 * An acquisition failure supplies NULL. No reader calls this function. */
void hub_imu_process(const struct bp_imu_sample *sample, const int16_t data[6]);
