// SPDX-License-Identifier: Apache-2.0
#pragma once

/* The controller header includes port declarations. No PBSYS service runs;
 * in particular, exclude the optional non-free telemetry implementation. */
#define PBSYS_CONFIG_TELEMETRY (0)
#define PBSYS_CONFIG_STATUS_LIGHT (0)

/* These declarations connect the unchanged hub_menu to Brickphyr bindings;
 * no PBIO matrix object or PBSYS stop-button worker is used. */
#ifdef CONFIG_BRICKPHYR_NATIVE
#define PBSYS_CONFIG_PROGRAM_STOP (1)
#define PBSYS_CONFIG_HUB_LIGHT_MATRIX (1)
#endif
