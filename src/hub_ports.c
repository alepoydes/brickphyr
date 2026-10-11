// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors
/* Electrical tests ported from lib/pbio/src/port_dcm_pup.c at a7b1b04ae.
 * Zephyr GPIO and a single delayed work item replace the PBIO scheduler.
 * See docs/ports.md for provenance, settling and identification limits. */
#include "hub_ports.h"
#include "port_uart.h"
#include <lego/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define PORTS DT_NODELABEL(hub_ports)
#define SETTLE_MS 2
#define MATCH_COUNT 20
#define RETRY_MS 100

enum pin { P5, P6, TX, RX, BUFFER, PIN_COUNT };
enum step { START, P6_HIGH, P6_LOW, TOUCH, ID1_HIGH, ID1_FLOAT, ID1_DONE,
	P5_HIGH, P5_LOW, RX_HIGH, RX_LOW, FINISH, UART };
enum group { GND, VCC, PULL_DOWN, OPEN };

static const uint16_t type_ids[3][3] = {
	{LEGO_DEVICE_TYPE_ID_LPF2_POWER, LEGO_DEVICE_TYPE_ID_LPF2_TURN,
	 LEGO_DEVICE_TYPE_ID_LPF2_LIGHT2},
	{LEGO_DEVICE_TYPE_ID_LPF2_TRAIN, LEGO_DEVICE_TYPE_ID_LPF2_LMOTOR,
	 LEGO_DEVICE_TYPE_ID_LPF2_LIGHT1},
	{LEGO_DEVICE_TYPE_ID_LPF2_MMOTOR, LEGO_DEVICE_TYPE_ID_LPF2_XMOTOR,
	 LEGO_DEVICE_TYPE_ID_LPF2_LIGHT},
};

static const struct gpio_dt_spec supply = GPIO_DT_SPEC_GET(PORTS, vcc_gpios);
#define PORT_PINS(node) [DT_REG_ADDR(node)] = { \
	GPIO_DT_SPEC_GET(node, p5_gpios), GPIO_DT_SPEC_GET(node, p6_gpios), \
	GPIO_DT_SPEC_GET(node, tx_gpios), GPIO_DT_SPEC_GET(node, rx_gpios), \
	GPIO_DT_SPEC_GET(node, buffer_gpios)},
static const struct gpio_dt_spec pins[BP_PORT_COUNT][PIN_COUNT] = {
	DT_FOREACH_CHILD_STATUS_OKAY(PORTS, PORT_PINS)
};
#define CHECK_INDEX(node) BUILD_ASSERT(DT_REG_ADDR(node) < BP_PORT_COUNT);
DT_FOREACH_CHILD_STATUS_OKAY(PORTS, CHECK_INDEX)

static struct port {
	struct bp_port_state value;
	int64_t next_ms;
	enum step step;
	enum group group;
	uint16_t candidate, previous;
	uint8_t matches, pin_value, pressed;
} ports[BP_PORT_COUNT];
static struct k_spinlock lock;
static atomic_t stopping;
static void scan(struct k_work *item);
static K_WORK_DELAYABLE_DEFINE(work, scan);

static int set_pin(uint32_t port, enum pin pin, gpio_flags_t flags)
{
	return gpio_pin_configure_dt(&pins[port][pin], flags);
}

/* Try all releases even after one fails. Disable the buffer before releasing
 * TX/RX; a failed detection must not leave a test output driven indefinitely. */
static int release_pins(uint32_t port)
{
	int result = gpio_is_ready_dt(&pins[port][BUFFER]) ?
		set_pin(port, BUFFER, GPIO_OUTPUT_INACTIVE) : -ENODEV;
	for (enum pin pin = P5; pin < BUFFER; pin++) {
		int err = gpio_is_ready_dt(&pins[port][pin]) ?
			set_pin(port, pin, GPIO_INPUT) : -ENODEV;
		if (!result) {
			result = err;
		}
	}
	return result;
}

static void publish(uint32_t port, int error, int cleanup, const struct bp_port_state *uart)
{
	struct port *p = &ports[port];
	enum bp_port_phase phase = error ? BP_PORT_FAULT : uart ? uart->phase :
		p->matches < MATCH_COUNT ? BP_PORT_DETECTING :
		p->previous == LEGO_DEVICE_TYPE_ID_NONE ? BP_PORT_EMPTY :
		p->previous == LEGO_DEVICE_TYPE_ID_LPF2_UNKNOWN_UART ? BP_PORT_UART : BP_PORT_PASSIVE;
	uint16_t id = error ? 0 : uart ? uart->type_id :
		phase == BP_PORT_PASSIVE || phase == BP_PORT_UART ? p->previous : 0;
	int64_t now = k_uptime_get();
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bp_port_state v = uart ? *uart : (struct bp_port_state){0};
	v.generation = p->value.generation;
	v.changed_ms = p->value.changed_ms;
	if (p->value.phase != phase || p->value.type_id != id) {
		v.changed_ms = now;
		v.generation++;
	}
	v.phase = phase;
	v.type_id = id;
	v.pressed = id == LEGO_DEVICE_TYPE_ID_LPF2_TOUCH ? p->pressed : 0;
	v.sample_ms = now;
	v.error = error ? error : uart ? uart->error : phase == BP_PORT_DETECTING ? -EAGAIN : 0;
	v.cleanup_error = cleanup;
	v.failures = p->value.failures;
	v.sequence = p->value.sequence;
	if (error) {
		v.failures += v.failures != UINT32_MAX;
	} else {
		v.sequence += uart ? uart->data_sequence - p->value.data_sequence : 1;
	}
	p->value = v;
	k_spin_unlock(&lock, key);
}

#define TRY_IO(operation) do { int err_ = (operation); if (err_ < 0) { return err_; } } while (0)

/* Each return schedules at least 2 ms of settling. Reads never sleep or busy
 * wait, so six ports do not create six worker stacks or serialize their waits. */
static int advance(uint32_t port)
{
	struct port *p = &ports[port];
	int value;
	switch (p->step) {
	case UART: {
		struct bp_port_state uart = {0};
		TRY_IO(port_uart_poll(port, &uart));
		publish(port, 0, 0, &uart);
		return 0;
	}
	case START:
		TRY_IO(release_pins(port));
		p->matches = 0;
		p->previous = LEGO_DEVICE_TYPE_ID_NONE;
		break;
	case P6_HIGH:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P6]));
		p->pin_value = value;
		TRY_IO(set_pin(port, TX, GPIO_OUTPUT_LOW));
		p->step = P6_LOW;
		return 0;
	case P6_LOW:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P6]));
		if (p->pin_value == 1 && value == 0) {
			p->candidate = LEGO_DEVICE_TYPE_ID_LPF2_TOUCH;
			TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_INACTIVE));
			TRY_IO(set_pin(port, TX, GPIO_INPUT));
			p->step = TOUCH;
		} else if (p->pin_value == 0 && value == 1) {
			p->candidate = LEGO_DEVICE_TYPE_ID_LPF2_TPOINT;
			p->step = FINISH;
		} else {
			TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
			p->pin_value = value;
			TRY_IO(set_pin(port, TX, GPIO_OUTPUT_HIGH));
			p->step = ID1_HIGH;
		}
		return 0;
	case TOUCH:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
		p->pressed = !value;
		p->step = FINISH;
		return 0;
	case ID1_HIGH:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
		if (p->pin_value == value) {
			p->group = value ? VCC : GND;
			p->step = ID1_DONE;
		} else {
			TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_INACTIVE));
			TRY_IO(set_pin(port, TX, GPIO_INPUT));
			p->step = ID1_FLOAT;
		}
		return 0;
	case ID1_FLOAT:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
		p->group = value ? OPEN : PULL_DOWN;
		p->step = ID1_DONE;
		return 0;
	case ID1_DONE:
		TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_INACTIVE));
		TRY_IO(set_pin(port, TX, GPIO_INPUT));
		TRY_IO(set_pin(port, P6, GPIO_OUTPUT_HIGH));
		p->step = P5_HIGH;
		return 0;
	case P5_HIGH:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
		p->pin_value = value;
		TRY_IO(set_pin(port, P6, GPIO_OUTPUT_LOW));
		p->step = P5_LOW;
		return 0;
	case P5_LOW:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][P5]));
		if (p->pin_value == 1 && value == 0) {
			if (p->group == OPEN) {
				p->candidate = LEGO_DEVICE_TYPE_ID_LPF2_3_PART;
			}
			p->step = FINISH;
		} else if (p->pin_value == 0 && value == 1) {
			p->candidate = LEGO_DEVICE_TYPE_ID_LPF2_EXPLOD;
			p->step = FINISH;
		} else {
			TRY_IO(set_pin(port, TX, GPIO_OUTPUT_HIGH));
			TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_ACTIVE));
			TRY_IO(set_pin(port, P6, GPIO_OUTPUT_HIGH));
			p->step = RX_HIGH;
		}
		return 0;
	case RX_HIGH:
		TRY_IO(value = gpio_pin_get_dt(&pins[port][RX]));
		if (value) {
			TRY_IO(set_pin(port, P6, GPIO_OUTPUT_LOW));
			/* Preserve Pybricks' extra discharge of RX capacitance. */
			TRY_IO(set_pin(port, RX, GPIO_OUTPUT_LOW));
			p->step = RX_LOW;
		} else {
			p->candidate = p->group < OPEN ? type_ids[p->group][GND] :
				LEGO_DEVICE_TYPE_ID_LPF2_UNKNOWN_UART;
			p->step = FINISH;
		}
		return 0;
	case RX_LOW:
		TRY_IO(set_pin(port, RX, GPIO_INPUT));
		TRY_IO(value = gpio_pin_get_dt(&pins[port][RX]));
		if (p->group < OPEN) {
			p->candidate = type_ids[p->group][value ? VCC : PULL_DOWN];
		}
		p->step = FINISH;
		return 0;
	case FINISH:
		if (p->candidate == p->previous) {
			p->matches += p->matches < MATCH_COUNT;
		} else {
			p->matches = 0;
			p->previous = p->candidate;
		}
		publish(port, 0, 0, NULL);
		if (p->matches == MATCH_COUNT && p->previous == LEGO_DEVICE_TYPE_ID_LPF2_UNKNOWN_UART &&
		    port_uart_available(port)) {
			TRY_IO(release_pins(port));
			TRY_IO(port_uart_start(port));
			TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_ACTIVE));
			p->step = UART;
			return 0;
		}
		break;
	}
	TRY_IO(set_pin(port, P6, GPIO_INPUT));
	TRY_IO(set_pin(port, TX, GPIO_OUTPUT_HIGH));
	TRY_IO(set_pin(port, BUFFER, GPIO_OUTPUT_ACTIVE));
	p->candidate = LEGO_DEVICE_TYPE_ID_NONE;
	p->group = OPEN;
	p->step = P6_HIGH;
	return 0;
}
#undef TRY_IO

static void scan(struct k_work *item)
{
	int64_t next = INT64_MAX;
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!pins[i][P5].port) {
			continue;
		}
		if (k_uptime_get() >= ports[i].next_ms) {
			int err = advance(i);
			if (err) {
				port_uart_stop(i);
				int cleanup = release_pins(i);
				publish(i, err, cleanup, NULL);
				ports[i].step = START;
			}
			ports[i].next_ms = k_uptime_get() + (err ? RETRY_MS : SETTLE_MS);
		}
		next = MIN(next, ports[i].next_ms);
	}
	if (!atomic_get(&stopping) && next != INT64_MAX) {
		/* Do not catch up with back-to-back work if a round ran late. */
		k_work_schedule(&work, K_MSEC(MAX(SETTLE_MS, next - k_uptime_get())));
	}
}

static int initialize(void)
{
	int err = gpio_is_ready_dt(&supply) ?
		gpio_pin_configure_dt(&supply, GPIO_OUTPUT_INACTIVE) : -ENODEV;
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		ports[i].value.error = -EAGAIN;
		if (!pins[i][P5].port) {
			continue;
		}
		bool ready = true;
		for (enum pin pin = P5; pin < PIN_COUNT; pin++) {
			ready &= gpio_is_ready_dt(&pins[i][pin]);
		}
		int reset = ready ? release_pins(i) : -ENODEV;
		if (!err) {
			err = reset;
		}
	}
	if (!err) {
		err = gpio_pin_set_dt(&supply, 1);
	}
	if (err) {
		int cleanup = gpio_is_ready_dt(&supply) ? gpio_pin_set_dt(&supply, 0) : 0;
		for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
			publish(i, err, cleanup, NULL);
		}
		return err;
	}
	k_work_schedule(&work, K_MSEC(SETTLE_MS));
	return 0;
}
SYS_INIT(initialize, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int hub_port_get(uint32_t port, struct bp_port_state *value)
{
	if (port >= BP_PORT_COUNT || !value) {
		return -EINVAL;
	}
	if (!pins[port][P5].port) {
		return -ENODEV;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	*value = ports[port].value;
	k_spin_unlock(&lock, key);
	/* Longest probe: ten settling intervals. Include tick rounding at
	 * each wait; longer service starvation must not look like fresh identity. */
	int64_t max_age = 10 * (SETTLE_MS + k_ticks_to_ms_ceil64(2));
	if (!value->error && k_uptime_get() - value->sample_ms > max_age) {
		value->error = -ETIMEDOUT;
	}
	return 0;
}

int hub_ports_shutdown(void)
{
	atomic_set(&stopping, 1);
	struct k_work_sync sync;
	k_work_cancel_delayable_sync(&work, &sync);
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		port_uart_stop(i);
	}
	int supply_error = gpio_is_ready_dt(&supply) ? gpio_pin_set_dt(&supply, 0) : -ENODEV;
	int result = supply_error;
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!pins[i][P5].port) {
			continue;
		}
		int err = release_pins(i);
		if (!result) {
			result = err;
		}
		k_spinlock_key_t key = k_spin_lock(&lock);
		struct bp_port_state *v = &ports[i].value;
		int cleanup = supply_error ? supply_error : err;
		enum bp_port_phase phase = cleanup ? BP_PORT_FAULT : BP_PORT_OFF;
		if (v->phase != phase || v->type_id) {
			v->generation++;
			v->changed_ms = k_uptime_get();
		}
		v->phase = phase;
		v->type_id = 0;
		v->pressed = 0;
		v->length = 0;
		if (!v->error || v->error == -EAGAIN) {
			v->error = cleanup ? cleanup : -ESHUTDOWN;
		}
		v->cleanup_error = cleanup;
		k_spin_unlock(&lock, key);
	}
	return result;
}
