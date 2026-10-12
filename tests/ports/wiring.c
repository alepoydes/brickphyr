// SPDX-License-Identifier: Apache-2.0
/* Synthetic passive loads and wire levels, not a model of analogue settling
 * or the STM32 GPIO peripheral. Also reused by the protected ARM SDK fixture. */
#define DT_DRV_COMPAT vnd_pup_test_gpio
#include "wiring.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

struct pup_test_circuit pup_test_circuits[6];
struct wiring_config {
	struct gpio_driver_config common;
	uint32_t index;
};
struct wiring_data {
	struct gpio_driver_data common;
	gpio_flags_t flags[7];
	uint32_t outputs;
	int64_t changed_ms;
	bool read_failed;
};
static struct wiring_data state[7];

static int configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct wiring_config *cfg = dev->config;
	struct wiring_data *data = dev->data;
	if (pin >= 7 || flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) {
		return -EINVAL;
	}
	if (cfg->index < 6) {
		struct pup_test_circuit *c = &pup_test_circuits[cfg->index];
		c->writes++;
		if (c->supply_error && pin >= 5 && (flags & GPIO_OUTPUT_INIT_HIGH)) {
			int err = c->supply_error;
			c->supply_error = 0;
			return err;
		}
		if (c->coast_error && pin == 5 && (flags & GPIO_OUTPUT_INIT_LOW)) {
			int err = c->coast_error;
			c->coast_error = 0;
			return err;
		}
		if (data->read_failed && c->release_error && pin == 4 && (flags & GPIO_OUTPUT_INIT_HIGH)) {
			return c->release_error;
		}
	}
	if ((pin == 1 || pin == 2) && (flags & GPIO_OUTPUT)) {
		data->changed_ms = k_uptime_get();
	}
	uint32_t previous = data->outputs;
	data->flags[pin] = flags;
	if (flags & GPIO_OUTPUT_INIT_HIGH) {
		data->outputs |= BIT(pin);
	} else if (flags & GPIO_OUTPUT_INIT_LOW) {
		data->outputs &= ~BIT(pin);
	}
	if (cfg->index < 6 && ((previous ^ data->outputs) & (BIT(5) | BIT(6)))) {
		pup_test_circuits[cfg->index].supply_changes++;
	}
	return 0;
}

static int get(const struct device *dev, gpio_port_value_t *value)
{
	const struct wiring_config *cfg = dev->config;
	struct wiring_data *d = dev->data;
	*value = d->outputs;
	if (cfg->index == 6) {
		return 0;
	}
	struct pup_test_circuit *c = &pup_test_circuits[cfg->index];
	c->reads++;
	d->read_failed = c->read_error != 0;
	if (c->read_error) {
		return c->read_error;
	}
	if (k_uptime_get() - d->changed_ms < 2) {
		return -EWOULDBLOCK;
	}
	bool tx_enabled = (d->flags[2] & GPIO_OUTPUT) && !(d->outputs & BIT(4));
	bool tx = d->outputs & BIT(2);
	bool p6_driven = d->flags[1] & GPIO_OUTPUT;
	bool p6 = p6_driven ? !!(d->outputs & BIT(1)) : false;
	bool p5;
	switch (c->kind) {
	case MEDIUM_MOTOR: /* ID1 pull-down, ID2 grounded. */
		p5 = tx_enabled && tx;
		p6 = false;
		break;
	case TRAIN_MOTOR: /* ID1 VCC, ID2 grounded. */
		p5 = true;
		p6 = false;
		break;
	case LIGHT: /* Both IDs pulled down; a driven pin overrides the pull. */
		p5 = tx_enabled && tx;
		break;
	case UART_DEVICE: /* ID1 open, ID2 grounded until UART handshake. */
		p5 = !tx_enabled || tx;
		p6 = false;
		break;
	case TOUCH_SWITCH: /* ID2 follows driven ID1, switch grounds released ID1. */
		p5 = tx_enabled ? tx : !c->pressed;
		p6 = tx_enabled ? tx : p5;
		break;
	default: /* Hub pull-up on undriven ID1. */
		p5 = !tx_enabled || tx;
		break;
	}
	*value = (p5 ? BIT(0) : 0) | (p6 ? BIT(1) | BIT(3) : 0);
	return 0;
}

static int set_masked(const struct device *dev, gpio_port_pins_t mask, gpio_port_value_t value)
{
	struct wiring_data *data = dev->data;
	data->outputs = (data->outputs & ~mask) | (value & mask);
	return 0;
}
static int set_bits(const struct device *dev, gpio_port_pins_t pins)
{
	return set_masked(dev, pins, pins);
}
static int clear_bits(const struct device *dev, gpio_port_pins_t pins)
{
	return set_masked(dev, pins, 0);
}
static const struct gpio_driver_api api = {
	.pin_configure = configure, .port_get_raw = get,
	.port_set_masked_raw = set_masked, .port_set_bits_raw = set_bits,
	.port_clear_bits_raw = clear_bits,
};

#define WIRING_DEFINE(inst) \
	static const struct wiring_config config_##inst = { \
		.common = {.port_pin_mask = BIT_MASK(DT_INST_PROP(inst, ngpios))}, \
		.index = DT_INST_PROP(inst, port_index)}; \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &state[DT_INST_PROP(inst, port_index)], \
		&config_##inst, POST_KERNEL, CONFIG_GPIO_INIT_PRIORITY, &api);
DT_INST_FOREACH_STATUS_OKAY(WIRING_DEFINE)

bool pup_test_supply(void) { return state[6].outputs & BIT(0); }
uint8_t pup_test_sensor_supply(uint32_t port) { return (state[port].outputs >> 5) & 3; }

bool pup_test_released(uint32_t port)
{
	struct wiring_data *d = &state[port];
	for (uint32_t pin = 0; pin < 4; pin++) {
		if (d->flags[pin] != GPIO_INPUT) {
			return false;
		}
	}
	return (d->flags[4] & GPIO_OUTPUT) && (d->outputs & BIT(4)) && !pup_test_sensor_supply(port);
}
