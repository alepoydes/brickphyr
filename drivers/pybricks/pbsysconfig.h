// SPDX-License-Identifier: Apache-2.0
#pragma once

/* The controller header includes port declarations. No PBSYS service runs;
 * in particular, exclude the optional non-free telemetry implementation. */
#define PBSYS_CONFIG_TELEMETRY (0)
#define PBSYS_CONFIG_STATUS_LIGHT (0)
