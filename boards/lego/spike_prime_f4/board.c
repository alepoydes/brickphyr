// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

void board_early_init_hook(void)
{
	const uint32_t pin = BIT(DT_GPIO_HOG_PIN_BY_IDX(DT_NODELABEL(power_hold), 0));

	/* Hold battery power before device initialization or waiting for USB. */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
	LL_GPIO_SetOutputPin(GPIOA, pin);
	LL_GPIO_SetPinMode(GPIOA, pin, LL_GPIO_MODE_OUTPUT);
}

/* Format and offsets are defined by the existing Prime F4 mboot variants. */
struct boot_info {
	uint8_t color;
	char mcu[15];
	uint8_t reserved;
	char board[15];
};

_Static_assert(sizeof(struct boot_info) == 32, "Prime F4 boot info must occupy 32 bytes");

#define BOOT_INFO { .color = 0x0d, .mcu = "STM32F413", .board = "SPIKE Prime" }

static const struct boot_info info1 __used __section(".boot_info1") = BOOT_INFO;
static const struct boot_info info2 __used __section(".boot_info2") = BOOT_INFO;
