// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2022 The Pybricks Authors
/* Selected H-bridge behavior from motor_driver_hbridge_pwm.c at a7b1b04ae.
 * Zephyr GPIO, PWM and pinctrl replace PBIO I/O. See docs/ports.md. */
#define __ZEPHYR_SUPERVISOR__
#include "output.h"
#include "hub_clock.h"
#include <brickphyr_port.h>
#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>

#define PORTS DT_NODELABEL(hub_ports)
#if defined(CONFIG_BRICKPHYR_MOTORS) && defined(CONFIG_PINCTRL)
#include <zephyr/drivers/pinctrl.h>
#define PINCTRL_STATE_REVERSE PINCTRL_STATE_PRIV_START
#define DEFINE_PINS(node) COND_CODE_1(DT_NODE_HAS_PROP(node, pinctrl_0), (PINCTRL_DT_DEFINE(node);), ())
DT_FOREACH_CHILD_STATUS_OKAY(PORTS, DEFINE_PINS)
#define MUX(node) COND_CODE_1(DT_NODE_HAS_PROP(node, pinctrl_0), (PINCTRL_DT_DEV_CONFIG_GET(node)), (NULL))
#else
#define MUX(node) NULL
#ifdef CONFIG_BRICKPHYR_MOTORS
#define CHECK_PINS(node) BUILD_ASSERT(!DT_NODE_HAS_PROP(node, pinctrl_0), "Motor pinmux requires CONFIG_PINCTRL");
DT_FOREACH_CHILD_STATUS_OKAY(PORTS, CHECK_PINS)
#endif
#endif
#ifdef CONFIG_BRICKPHYR_MOTORS
#define CHECK_PWM(node) COND_CODE_1(DT_NODE_HAS_PROP(node, pwms), \
	(BUILD_ASSERT(DT_PROP_LEN(node, pwms) == 2, "Motor needs two PWM channels");), ())
DT_FOREACH_CHILD_STATUS_OKAY(PORTS, CHECK_PWM)
#define PWMS(node) {PWM_DT_SPEC_GET_BY_IDX_OR(node, 0, {0}), PWM_DT_SPEC_GET_BY_IDX_OR(node, 1, {0})}
#else
#define PWMS(node) {{0}, {0}}
#endif
#define OUTPUT(node) [DT_REG_ADDR(node)] = { \
	.pins = {GPIO_DT_SPEC_GET_OR(node, p1_gpios, {0}), GPIO_DT_SPEC_GET_OR(node, p2_gpios, {0})}, \
	.pwm = PWMS(node), .mux = MUX(node)},
static const struct {
	struct gpio_dt_spec pins[2];
	struct pwm_dt_spec pwm[2];
	const struct pinctrl_dev_config *mux;
} outputs[BP_PORT_COUNT] = {DT_FOREACH_CHILD_STATUS_OKAY(PORTS, OUTPUT)};
/* A same-direction update touches only the PWM compare register. */
static int8_t direction[BP_PORT_COUNT];

int port_output_coast(uint32_t port)
{
	if (port >= BP_PORT_COUNT) return -EINVAL;
	direction[port] = 0;
	int result = 0;
	for (size_t i = 0; i < 2; i++) {
		const struct gpio_dt_spec *pin = &outputs[port].pins[i];
		if (!pin->port) continue;
		int err = gpio_is_ready_dt(pin) ? gpio_pin_configure_dt(pin, GPIO_OUTPUT_INACTIVE) : -ENODEV;
		if (!result) result = err;
	}
#ifdef CONFIG_BRICKPHYR_MOTORS
	/* GPIO has disconnected the timer. Disable available channels as well:
	 * re-enabling an STM32 channel latches a new compare before remuxing it.
	 * Coast itself does not need a working PWM controller. */
	for (size_t i = 0; i < 2; i++) {
		const struct pwm_dt_spec *pwm = &outputs[port].pwm[i];
		if (!pwm->dev || !pwm_is_ready_dt(pwm)) continue;
		int err = pwm_set_cycles(pwm->dev, pwm->channel, 0, 0, pwm->flags);
		if (!result) result = err;
	}
#endif
	return result;
}

static int pins_ready(uint32_t port)
{
	if (port >= BP_PORT_COUNT) return -EINVAL;
	for (size_t i = 0; i < 2; i++) {
		const struct gpio_dt_spec *pin = &outputs[port].pins[i];
		if (!pin->port) return -ENOTSUP;
		if (!gpio_is_ready_dt(pin)) return -ENODEV;
	}
	return 0;
}

int port_output_supply(uint32_t port, uint8_t pin)
{
	if (pin > 2) return -EINVAL;
	int err = pins_ready(port);
	if (err) return err;
	err = port_output_coast(port);
	return err || !pin ? err : gpio_pin_configure_dt(&outputs[port].pins[pin - 1], GPIO_OUTPUT_ACTIVE);
}

#ifdef CONFIG_BRICKPHYR_MOTORS
int port_output_check(uint32_t port)
{
	int err = pins_ready(port);
	if (err) return err;
	for (size_t i = 0; i < 2; i++) {
		if (!outputs[port].pwm[i].dev) return -ENOTSUP;
		if (!pwm_is_ready_dt(&outputs[port].pwm[i])) return -ENODEV;
	}
	return 0;
}

int port_output_drive(uint32_t port, int32_t duty)
{
	if (duty < -1000 || duty > 1000) return -EINVAL;
	int err = port_output_check(port);
	if (err) return err;
	int sign = duty > 0 ? 1 : duty < 0 ? -1 : 0;
	if (!sign) {
		/* Brake overrides PWM with both GPIO outputs high, as in PBIO. */
		direction[port] = 0;
		err = gpio_pin_configure_dt(&outputs[port].pins[0], GPIO_OUTPUT_ACTIVE);
		int second = gpio_pin_configure_dt(&outputs[port].pins[1], GPIO_OUTPUT_ACTIVE);
		return err ? err : second;
	}
	uint32_t channel = sign > 0 ? 0 : 1;
	const struct pwm_dt_spec *pwm = &outputs[port].pwm[channel];
	uint64_t rate;
	err = pwm_get_cycles_per_sec(pwm->dev, pwm->channel, &rate);
	if (err) return err;
	if (!rate || !pwm->period || rate > UINT64_MAX / pwm->period) return -ERANGE;
	uint64_t cycles = rate * pwm->period / NSEC_PER_SEC;
	if (!cycles || cycles > UINT32_MAX) return -ERANGE;
	bool change = direction[port] != sign;
	if (change && (err = port_output_coast(port))) return err;
	/* Convert once into timer ticks; rounding the pulse in nanoseconds first
	 * would lose one of the F4's 1000 duty steps. */
	err = pwm_set_cycles(pwm->dev, pwm->channel, cycles,
		cycles * (sign > 0 ? duty : -duty) / 1000, pwm->flags);
	if (!err && change) {
		err = gpio_pin_configure_dt(&outputs[port].pins[1 - channel], GPIO_OUTPUT_ACTIVE);
#ifdef CONFIG_PINCTRL
		if (!err && outputs[port].mux) {
			err = pinctrl_apply_state(outputs[port].mux,
				channel ? PINCTRL_STATE_REVERSE : PINCTRL_STATE_DEFAULT);
		}
#endif
	}
	direction[port] = err ? 0 : sign;
	return err;
}

int port_output_group(const struct bp_motor_group_command *command,
	struct port_output_result *result, int (*check)(void))
{
	*result = (struct port_output_result){0};
	/* This portable backend has sequential GPIO/PWM operations. A common call
	 * boundary does not claim simultaneous electrical switching. */
	bool cleanup = command->coast_mask == command->port_mask;
	int first = 0;
	for (uint32_t port = 0; port < BP_PORT_COUNT; port++) {
		if (!(command->port_mask & BIT(port))) continue;
		bool passive = command->coast_mask & BIT(port);
		int err = check ? check() : 0;
		if (!err && !passive && command->deadline_ms && k_uptime_get() >= command->deadline_ms) err = -ETIMEDOUT;
		if (!err) err = passive ? port_output_coast(port) : port_output_drive(port, command->ports[port].duty_permille);
		result->errors[port] = err;
		if (!err) {
			result->applied_mask |= BIT(port);
			result->applied_us[port] = hub_time_us();
		}
		if (!first) first = err;
		if (err && !cleanup) break;
	}
	return first;
}
#endif
