// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <stdint.h>
#include <cmsis_core.h>
#include <zephyr/sys/reboot.h>
#include "hub.h"
#include "native.h"
#include "hub_sound.h"
#include <stm32_ll_bus.h>
#include <stm32_ll_gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#define DFU_REQUEST 0xAAAAAAAAU
#define MBOOT_ADDRESS (DT_REG_ADDR(DT_NODELABEL(flash0)) + \
		       DT_REG_ADDR(DT_NODELABEL(mboot_partition)))

/* Reserved SRAM survives reset and is not initialized by Zephyr or its allocator. */
static volatile uint32_t *const boot_request = (void *)DT_REG_ADDR(DT_NODELABEL(boot_request));

/* Pybricks/MicroPython mboot ABI: magic in r0, vector table in r1. */
static __attribute__((naked, noreturn)) void enter_mboot(uint32_t magic, uint32_t vector)
{
	__asm__ volatile (
		"movs r2, #0\n"
		"msr control, r2\n"
		"isb\n"
		"msr basepri, r2\n"
		"ldr r2, [r1]\n"
		"msr msp, r2\n"
		"ldr r2, [r1, #4]\n"
		"cpsie i\n"
		"bx r2\n"
	);
}

int hub_reboot_dfu(void)
{
	const uint32_t *vectors = (void *)MBOOT_ADDRESS;
	uint32_t reset = vectors[1];
	uint32_t stack = vectors[0];

	if (stack <= 0x20000000U || stack > 0x20050000U || (stack & 7U) ||
	    !(reset & 1U) || reset < MBOOT_ADDRESS ||
	    reset >= MBOOT_ADDRESS + DT_REG_SIZE(DT_NODELABEL(mboot_partition))) {
		return -ENOEXEC;
	}
	int err = native_shutdown();
	if (!err) {
		err = hub_sound_shutdown();
	}
	if (err) {
		return err;
	}
	boot_request[0] = DFU_REQUEST;
	boot_request[1] = ~DFU_REQUEST;
	__DSB();
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

void hub_power_release(void)
{
	/* A held center button may keep power on until released, as in Pybricks. */
	LL_GPIO_ResetOutputPin(GPIOA, BIT(DT_GPIO_HOG_PIN_BY_IDX(DT_NODELABEL(power_hold), 0)));
}

void board_early_init_hook(void)
{
	bool dfu = boot_request[0] == DFU_REQUEST && boot_request[1] == ~DFU_REQUEST;

	boot_request[0] = 0;
	boot_request[1] = 0;
	if (dfu) {
		/* Before clocks, device IRQs or scheduler start; undo core setup only. */
		SysTick->CTRL = 0;
		MPU->CTRL = 0;
		SCB->VTOR = MBOOT_ADDRESS;
		__DSB();
		__ISB();
		enter_mboot(0x70ad0000, MBOOT_ADDRESS);
	}
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
