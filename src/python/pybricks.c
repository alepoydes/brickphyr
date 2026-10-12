// SPDX-License-Identifier: Apache-2.0
#include "py/mphal.h"
#include <pbdrv/clock.h>

/* Pybricks tools use the same user-accessible monotonic clock as the VM. */
uint32_t pbdrv_clock_get_ms(void)
{
	return mp_hal_ticks_ms();
}

#ifdef CONFIG_BRICKPHYR_NATIVE
#include "devices.h"
#include <brickphyr.h>
#include <pbdrv/button.h>
#include <pbsys/program_stop.h>

static const pbio_button_flags_t buttons[] = {
	PBIO_BUTTON_CENTER, PBIO_BUTTON_LEFT, PBIO_BUTTON_RIGHT, PBIO_BUTTON_RIGHT_UP,
};

static pbio_button_flags_t button_flags(uint32_t mask)
{
	pbio_button_flags_t flags = 0;
	for (size_t i = 0; i < MP_ARRAY_SIZE(buttons); i++) {
		if (mask & (1U << i)) flags |= buttons[i];
	}
	return flags;
}

pbio_button_flags_t pbdrv_button_get_pressed(void)
{
	return button_flags(bp_buttons());
}

pbio_button_flags_t pbsys_program_stop_get_buttons(void)
{
	return button_flags(bp_get_stop_buttons());
}

void pbsys_program_stop_set_buttons(pbio_button_flags_t flags)
{
	uint32_t mask = 0;
	for (size_t i = 0; i < MP_ARRAY_SIZE(buttons); i++) {
		if (flags & buttons[i]) mask |= 1U << i;
	}
	python_device_check(bp_set_stop_buttons(mask));
}
#endif
