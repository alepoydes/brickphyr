// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2022 The Pybricks Authors
/* F4 H-bridge polarity/brake/coast follows motor_driver_hbridge_pwm.c at
 * a7b1b04ae. Timer ownership and grouped updates are Brickphyr-specific.
 * RM0430: TIM2 TRGO reaches TIM1/3/4 through ITR1. Reset mode resets CNT/PSC
 * even with UDIS set; UDIS gates the transfer from preloads to active compares.
 */
#define __ZEPHYR_SUPERVISOR__
#include "output.h"
#include "hub_clock.h"
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/reset.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_tim.h>

#define PORTS DT_NODELABEL(hub_ports)
#define MASTER DT_PHANDLE(PORTS, sync_timer)
#define TIMER(node) { .timer = (TIM_TypeDef *)DT_REG_ADDR(node), \
	.clocks = {STM32_CLOCK_INFO(0, node), STM32_CLOCK_INFO(1, node)}, \
	.reset = RESET_DT_SPEC_GET(node) }
static const struct {
	TIM_TypeDef *timer;
	struct stm32_pclken clocks[2];
	struct reset_dt_spec reset;
} timers[] = {
	TIMER(MASTER), TIMER(DT_NODELABEL(timers1)),
	TIMER(DT_NODELABEL(timers4)), TIMER(DT_NODELABEL(timers3)),
};
BUILD_ASSERT(DT_SAME_NODE(MASTER, DT_NODELABEL(timers2)));
BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(MASTER) == 0);
BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(timers1)) == 0);
BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(timers3)) == 0);
BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(timers4)) == 0);
/* F4 currently uses ordinary WFI idle, never STOP or peripheral suspension.
 * A future deep-sleep port needs explicit quiesce/resume before enabling PM. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_PM) && !IS_ENABLED(CONFIG_PM_DEVICE));

#define DEFINE_PINS(node) PINCTRL_DT_DEFINE(node);
DT_FOREACH_CHILD_STATUS_OKAY(PORTS, DEFINE_PINS)
#define OUTPUT(node) [DT_REG_ADDR(node)] = { \
	.timer = (TIM_TypeDef *)DT_REG_ADDR(DT_PARENT(DT_PWMS_CTLR_BY_IDX(node, 0))), \
	.channels = {DT_PWMS_CHANNEL_BY_IDX(node, 0) - 1, DT_PWMS_CHANNEL_BY_IDX(node, 1) - 1}, \
	.pins = {GPIO_DT_SPEC_GET(node, p1_gpios), GPIO_DT_SPEC_GET(node, p2_gpios)}, \
	.mux = PINCTRL_DT_DEV_CONFIG_GET(node)},
static const struct {
	TIM_TypeDef *timer;
	uint8_t channels[2];
	struct gpio_dt_spec pins[2];
	const struct pinctrl_dev_config *mux;
} outputs[BP_PORT_COUNT] = {DT_FOREACH_CHILD_STATUS_OKAY(PORTS, OUTPUT)};
#define CHECK_OUTPUT(node) \
	BUILD_ASSERT(DT_SAME_NODE(DT_PWMS_CTLR_BY_IDX(node, 0), DT_PWMS_CTLR_BY_IDX(node, 1))); \
	BUILD_ASSERT(DT_PWMS_FLAGS_BY_IDX(node, 0) == PWM_POLARITY_INVERTED); \
	BUILD_ASSERT(DT_PWMS_FLAGS_BY_IDX(node, 1) == PWM_POLARITY_INVERTED); \
	BUILD_ASSERT(DT_PWMS_CHANNEL_BY_IDX(node, 0) >= 1 && DT_PWMS_CHANNEL_BY_IDX(node, 0) <= 4); \
	BUILD_ASSERT(DT_PWMS_CHANNEL_BY_IDX(node, 1) >= 1 && DT_PWMS_CHANNEL_BY_IDX(node, 1) <= 4);
DT_FOREACH_CHILD_STATUS_OKAY(PORTS, CHECK_OUTPUT)

enum { COUNTER_HZ = 12000000, PERIOD = 1000, LOW = UINT16_MAX, HIGH = 0 };
/* Slave overflow must not race the master's reset edge. A larger ARR also
 * keeps constant-low CCR above ARR, including if the master stops. */
enum { SLAVE_RELOAD = LOW - 1, WAIT_US = 1000 };
static uint16_t active[BP_PORT_COUNT][2];
static int ready = -EAGAIN;

static void compare(uint32_t port, const uint16_t value[2])
{
	for (size_t i = 0; i < 2; i++) {
		volatile uint32_t *reg = (volatile uint32_t *)((uintptr_t)outputs[port].timer +
			offsetof(TIM_TypeDef, CCR1) + sizeof(uint32_t) * outputs[port].channels[i]);
		*reg = value[i];
	}
}

static int gpio_coast(uint32_t port)
{
	if (port >= BP_PORT_COUNT || !outputs[port].timer) return -EINVAL;
	int first = 0;
	for (size_t i = 0; i < 2; i++) {
		const struct gpio_dt_spec *pin = &outputs[port].pins[i];
		int err = gpio_is_ready_dt(pin) ? gpio_pin_configure_dt(pin, GPIO_OUTPUT_INACTIVE) : -ENODEV;
		if (!first) first = err;
	}
	return first;
}

/* Loss of timer progress makes every timer-owned output suspect. Disconnect
 * all pins, retain the failure and refuse subsequent drive until reset. Coast
 * remains available through GPIO even when timer initialization failed. */
static void timer_failed(void)
{
	ready = -EIO;
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) if (outputs[i].timer) (void)gpio_coast(i);
}

static void freeze(void)
{
	for (size_t i = 1; i < ARRAY_SIZE(timers); i++) LL_TIM_DisableUpdateEvent(timers[i].timer);
}

static bool latched(void)
{
	for (size_t i = 1; i < ARRAY_SIZE(timers); i++) {
		if (!LL_TIM_IsActiveFlag_UPDATE(timers[i].timer)) return false;
	}
	return true;
}

/* Called only by the serialized port owner. No IRQ runs every PWM cycle:
 * only a new command waits for its common edge. The CPU can be preempted
 * while waiting, but not during the short operation which arms all slaves. */
static int latch(uint32_t mask, const uint16_t values[BP_PORT_COUNT][2],
	int64_t deadline_ms, int (*check)(void), int64_t *completed_us)
{
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) if (mask & BIT(i)) compare(i, values[i]);
	uint32_t started = k_cycle_get_32();
	uint32_t limit = k_us_to_cyc_ceil32(WAIT_US);
	int err = 0;
	for (;;) {
		unsigned int key = irq_lock();
		if (check) err = check();
		uint32_t phase = LL_TIM_GetCounter(timers[0].timer);
		/* Arm after trigger propagation and with half a PWM period still
		 * available for the three APB writes. No driver call or allocation
		 * occurs after the phase check while interrupts are masked. */
		bool window = phase >= PERIOD / 4 && phase <= PERIOD / 2;
		if (!err && deadline_ms && (hub_time_us() +
		    DIV_ROUND_UP((PERIOD - MIN(phase, PERIOD)) * 1000000ULL, COUNTER_HZ) +
		    HUB_TIME_RESOLUTION_US) / 1000 >= deadline_ms) err = -ETIMEDOUT;
		if (!err && window) {
			for (size_t i = 1; i < ARRAY_SIZE(timers); i++) {
				LL_TIM_ClearFlag_UPDATE(timers[i].timer);
				LL_TIM_EnableUpdateEvent(timers[i].timer);
			}
		}
		irq_unlock(key);
		if (err || window) break;
		if ((uint32_t)(k_cycle_get_32() - started) >= limit) {
			timer_failed();
			err = -EIO;
			break;
		}
	}
	if (err) {
		/* UDIS still gates every slave. A rejected command must not become
		 * active when the next independent command enables shadow updates. */
		for (uint32_t i = 0; i < BP_PORT_COUNT; i++) if (mask & BIT(i)) compare(i, active[i]);
		return err;
	}
	started = k_cycle_get_32();
	while (!latched()) {
		if ((uint32_t)(k_cycle_get_32() - started) >= limit && !latched()) {
			freeze();
			timer_failed();
			return -EIO;
		}
	}
	freeze();
	*completed_us = hub_time_us();
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!(mask & BIT(i))) continue;
		active[i][0] = values[i][0];
		active[i][1] = values[i][1];
	}
	return 0;
}

static int polarity(const uint16_t value[2])
{
	return value[0] > value[1] ? 1 : value[0] < value[1] ? -1 : 0;
}

static int apply(uint32_t mask, const uint16_t values[BP_PORT_COUNT][2],
	int64_t deadline_ms, int (*check)(void), int64_t *completed_us)
{
	if (ready) return ready;
	uint32_t reverse = 0;
	uint16_t coast[BP_PORT_COUNT][2];
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!(mask & BIT(i))) continue;
		int previous = polarity(active[i]);
		if (previous && polarity(values[i]) == -previous) {
			reverse |= BIT(i);
			coast[i][0] = coast[i][1] = LOW;
		}
	}
	/* Preserve break-before-make on direction changes. The reversing legs
	 * coast for at least one PWM period before the whole requested group latches.
	 * Other members retain their previous output until that final edge. */
	int err = reverse ? latch(reverse, coast, deadline_ms, check, completed_us) : 0;
	return err ? err : latch(mask, values, deadline_ms, check, completed_us);
}

int port_output_check(uint32_t port)
{
	return port >= BP_PORT_COUNT || !outputs[port].timer ? -EINVAL : ready;
}

int port_output_coast(uint32_t port)
{
	if (port >= BP_PORT_COUNT || !outputs[port].timer) return -EINVAL;
	if (ready) return gpio_coast(port);
	uint16_t values[BP_PORT_COUNT][2];
	values[port][0] = values[port][1] = LOW;
	int64_t completed_us;
	int err = apply(BIT(port), values, 0, NULL, &completed_us);
	/* A timer error cannot prevent an attempt to disconnect this output. */
	return err ? gpio_coast(port) : 0;
}

int port_output_supply(uint32_t port, uint8_t pin)
{
	if (pin > 2) return -EINVAL;
	if (!pin) return port_output_coast(port);
	int err = port_output_check(port);
	if (err) return err;
	uint16_t values[BP_PORT_COUNT][2];
	values[port][0] = pin == 1 ? HIGH : LOW;
	values[port][1] = pin == 2 ? HIGH : LOW;
	int64_t completed_us;
	return apply(BIT(port), values, 0, NULL, &completed_us);
}

int port_output_group(const struct bp_motor_group_command *command,
	struct port_output_result *result, int (*check)(void))
{
	*result = (struct port_output_result){0};
	uint16_t values[BP_PORT_COUNT][2];
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!(command->port_mask & BIT(i))) continue;
		int32_t duty = command->ports[i].duty_permille;
		values[i][0] = duty > 0 ? duty == 1000 ? LOW : duty : HIGH;
		values[i][1] = duty < 0 ? duty == -1000 ? LOW : -duty : HIGH;
		if (command->coast_mask & BIT(i)) values[i][0] = values[i][1] = LOW;
	}
	int64_t completed_us = 0;
	int err = apply(command->port_mask, values, command->deadline_ms, check, &completed_us);
	bool cleanup = command->coast_mask == command->port_mask;
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!(command->port_mask & BIT(i))) continue;
		/* Emergency cleanup is deliberately independent of failed clocks. It
		 * reports each successful GPIO coast, without claiming a shared edge. */
		int output_error = err && cleanup && ready ? gpio_coast(i) : err;
		result->errors[i] = output_error;
		if (!output_error) {
			result->applied_mask |= BIT(i);
			result->applied_us[i] = err ? hub_time_us() : completed_us;
		}
	}
	return err;
}

int port_output_drive(uint32_t port, int32_t duty)
{
	if (duty < -1000 || duty > 1000) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	struct bp_motor_group_command command = {.port_mask = BIT(port)};
	command.ports[port].duty_permille = duty;
	struct port_output_result result;
	return port_output_group(&command, &result, NULL);
}

static int initialize(void)
{
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!outputs[i].timer) continue;
		int err = gpio_coast(i);
		if (err) return ready = err;
		active[i][0] = active[i][1] = LOW;
	}
	const struct device *clock = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
	if (!device_is_ready(clock)) return ready = -ENODEV;
	for (size_t i = 0; i < ARRAY_SIZE(timers); i++) {
		int err = clock_control_on(clock, (clock_control_subsys_t)&timers[i].clocks[0]);
		if (!err) err = reset_line_toggle_dt(&timers[i].reset);
		if (!err) err = clock_control_configure(clock, (clock_control_subsys_t)&timers[i].clocks[1], NULL);
		uint32_t rate = 0;
		if (!err) err = clock_control_get_rate(clock, (clock_control_subsys_t)&timers[i].clocks[1], &rate);
		if (err) return ready = err;
		if (!rate || rate % COUNTER_HZ || rate / COUNTER_HZ > UINT16_MAX + 1U) return ready = -ERANGE;
		TIM_TypeDef *timer = timers[i].timer;
		LL_TIM_SetPrescaler(timer, rate / COUNTER_HZ - 1);
		LL_TIM_SetAutoReload(timer, i ? SLAVE_RELOAD : PERIOD - 1);
		if (i) {
			const uint32_t channels[] = {LL_TIM_CHANNEL_CH1, LL_TIM_CHANNEL_CH2, LL_TIM_CHANNEL_CH3, LL_TIM_CHANNEL_CH4};
			for (size_t ch = 0; ch < ARRAY_SIZE(channels); ch++) {
				LL_TIM_OC_SetMode(timer, channels[ch], LL_TIM_OCMODE_PWM1);
				LL_TIM_OC_SetPolarity(timer, channels[ch], LL_TIM_OCPOLARITY_LOW);
				LL_TIM_OC_EnablePreload(timer, channels[ch]);
			}
			timer->CCR1 = timer->CCR2 = timer->CCR3 = timer->CCR4 = LOW;
			LL_TIM_CC_EnableChannel(timer, LL_TIM_CHANNEL_CH1 | LL_TIM_CHANNEL_CH2 | LL_TIM_CHANNEL_CH3 | LL_TIM_CHANNEL_CH4);
			if (IS_TIM_BREAK_INSTANCE(timer)) LL_TIM_EnableAllOutputs(timer);
		}
		LL_TIM_GenerateEvent_UPDATE(timer);
		LL_TIM_ClearFlag_UPDATE(timer);
		if (i) {
			LL_TIM_DisableUpdateEvent(timer);
			LL_TIM_SetTriggerInput(timer, LL_TIM_TS_ITR1);
			LL_TIM_SetSlaveMode(timer, LL_TIM_SLAVEMODE_RESET);
			LL_TIM_EnableCounter(timer);
		}
	}
	/* Do not rely on reset defaults for WFI: all four timers must continue
	 * together while the CPU sleeps. These bits do not enable STOP mode. */
	LL_APB1_GRP1_EnableClockLowPower(LL_APB1_GRP1_PERIPH_TIM2 |
		LL_APB1_GRP1_PERIPH_TIM3 | LL_APB1_GRP1_PERIPH_TIM4);
	LL_APB2_GRP1_EnableClockLowPower(LL_APB2_GRP1_PERIPH_TIM1);
	LL_TIM_SetTriggerOutput(timers[0].timer, LL_TIM_TRGO_UPDATE);
	LL_TIM_EnableCounter(timers[0].timer);
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!outputs[i].timer) continue;
		int err = pinctrl_apply_state(outputs[i].mux, PINCTRL_STATE_DEFAULT);
		if (err) {
			timer_failed();
			return ready = err;
		}
	}
	return ready = 0;
}
SYS_INIT(initialize, POST_KERNEL, CONFIG_PWM_INIT_PRIORITY);
