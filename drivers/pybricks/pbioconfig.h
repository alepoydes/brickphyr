// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Only portable computation and register helpers; no PBIO device adapter or
 * scheduler is linked. The acquisition worker owns the estimator. */
#define PBIO_CONFIG_IMU (1)
