// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>
#include <brickphyr_charger.h>

enum hub_charger_usb_event { HUB_CHARGER_USB_RESET, HUB_CHARGER_USB_CONFIGURED,
	HUB_CHARGER_USB_SUSPEND, HUB_CHARGER_USB_RESUME };

#ifdef CONFIG_BRICKPHYR_CHARGER
/* USB callback records state only; the hub loop owns all charger I/O. */
void hub_charger_usb(enum hub_charger_usb_event event);
void hub_charger_update(int vbus, int64_t now);
int hub_charger_get(struct bp_charger_state *value);
#else
static inline void hub_charger_usb(enum hub_charger_usb_event event) { }
static inline void hub_charger_update(int vbus, int64_t now) { }
static inline int hub_charger_get(struct bp_charger_state *value) { return -ENOTSUP; }
#endif
