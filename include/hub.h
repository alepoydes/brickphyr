// SPDX-License-Identifier: Apache-2.0
#ifndef BRICKPHYR_HUB_H_
#define BRICKPHYR_HUB_H_

#include <stdint.h>
#include <stddef.h>

/* Privileged F4 board operations; not an application ABI. */
int hub_reboot_dfu(void);
void hub_power_release(void);
int hub_bluetooth_power_off(void);
int hub_bluetooth_version(char *value, size_t size);
void hub_run(void);
uint32_t hub_buttons(void);

static inline const char *hub_name(void)
{
#ifdef CONFIG_BT
	return CONFIG_BT_DEVICE_NAME;
#else
	return "Brickphyr";
#endif
}

#endif
