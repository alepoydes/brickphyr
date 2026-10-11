// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Only portable computation and register helpers; no PBIO device adapter or
 * scheduler is linked. Brickphyr services own the estimators. */
#define PBIO_CONFIG_IMU (1)
#define PBIO_CONFIG_CONTROL_LOOP_TIME_MS (5)
#define PBIO_CONFIG_LOGGER (0)
