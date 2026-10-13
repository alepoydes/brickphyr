// SPDX-License-Identifier: Apache-2.0

#include <cmsis_core.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include "application.h"
#include "hub.h"
#include "hub_ports.h"

/* Factory loader ABI from Pybricks technic_hub/platform.ld and
 * drv/reset/reset_stm32.c. This word is excluded from runtime SRAM. */
static volatile uint32_t *const boot_request = (void *)DT_REG_ADDR(DT_NODELABEL(boot_request));

void board_early_init_hook(void)
{
	LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOC);
	LL_GPIO_SetOutputPin(GPIOC, LL_GPIO_PIN_12);
	LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_12, LL_GPIO_MODE_OUTPUT);
	/* A stale selector must not turn an unrelated reset into an update. */
	*boot_request = 0;
}

void hub_power_release(void)
{
	LL_GPIO_ResetOutputPin(GPIOC, LL_GPIO_PIN_12);
}

int hub_reboot_dfu(void)
{
	int err = application_shutdown();
	if (!err) err = hub_ports_shutdown();
	if (err) return err;
	*boot_request = 0xAAAAAAAA;
	__DSB();
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}
