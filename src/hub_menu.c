// SPDX-License-Identifier: Apache-2.0
#include "hub_menu.h"

#include <zephyr/dt-bindings/input/input-event-codes.h>

bool hub_menu_input(struct hub_menu *menu, uint16_t key, bool pressed,
		    enum host_mode current)
{
	if (key == INPUT_KEY_BLUETOOTH && pressed) {
		menu->open = !menu->open;
		menu->selected = current;
		menu->confirm_armed = false;
	} else if (menu->open) {
		if ((key == INPUT_KEY_LEFT || key == INPUT_KEY_RIGHT) && pressed) {
			menu->selected = menu->selected == HOST_PYBRICKS ? HOST_SHELL : HOST_PYBRICKS;
		} else if (key == INPUT_KEY_ENTER) {
			bool confirm = !pressed && menu->confirm_armed;
			menu->confirm_armed = pressed;
			return confirm;
		}
	}
	return false;
}
