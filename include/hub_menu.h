// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host_transport.h"

struct hub_menu {
	bool open;
	bool confirm_armed;
	enum host_mode selected;
};

/* True requests confirmation; the caller closes the menu only after selection
 * succeeds. Holding center remains the power controller's responsibility. */
bool hub_menu_input(struct hub_menu *menu, uint16_t key, bool pressed,
		    enum host_mode current);
/* A running program or management owner closes and disables system selection. */
bool hub_menu_idle(struct hub_menu *menu, bool available);
