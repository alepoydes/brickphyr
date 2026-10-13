// SPDX-License-Identifier: Apache-2.0
#include <brickphyr.h>
#include <zephyr/llext/symbol.h>

/* Compiled with __ZEPHYR_USER__: these wrappers unconditionally enter SVC.
 * Take addresses of the generated wrappers, never privileged implementations.
 * A call from an application therefore still enters the verifier through SVC.
 * This table and its wrappers reside in user-readable/executable RO flash. */
#define CAP(name) (IS_ENABLED(CONFIG_BRICKPHYR_##name) ? BP_CAP_##name : 0)
const struct bp_api bp_api_v1 = {
	.capabilities = CAP(LIGHT) | CAP(MATRIX) | CAP(IMU) | CAP(SOUND) |
		CAP(BATTERY) | CAP(CHARGER) | CAP(PORTS) | CAP(PORT_UART) |
		CAP(MOTORS) | CAP(MOTOR_CONTROL) | CAP(RADIO) | CAP(BLE_PEER) |
		CAP(HUB_NETWORK) | CAP(APP_STORAGE),
#include <brickphyr_api.inc>
};
#undef CAP

EXPORT_GROUP_SYMBOL(BRICKPHYR, bp_api_v1);
