// SPDX-License-Identifier: Apache-2.0
#include "port_uart.h"
#include "lump.h"
#include "sensor.h"
#include <string.h>
#include <zephyr/drivers/uart.h>
#if defined(CONFIG_PINCTRL)
#include <zephyr/drivers/pinctrl.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>

#define PORTS DT_NODELABEL(hub_ports)
#define IO_TIMEOUT_MS 250
#define KEEPALIVE_MS 100
/* 16 modes, 14 INFO types, largest frame at the slowest baud + startup.
 * Inactivity still fails after 250 ms, including during TYPE discovery. */
#define SYNC_TIMEOUT_MS (16 * 14 * 35 * 10 * 1000 / 2400 + 1000)
#define RX_CAPACITY 512
#define RX_BUDGET 128

#if defined(CONFIG_PINCTRL)
#define DECLARE_PINS(node) COND_CODE_1(DT_NODE_HAS_PROP(DT_PHANDLE(node, uart), pinctrl_0), \
	(PINCTRL_DT_DEV_CONFIG_DECLARE(DT_PHANDLE(node, uart));), ())
#define MAY_DECLARE(node) COND_CODE_1(DT_NODE_HAS_PROP(node, uart), (DECLARE_PINS(node)), ())
DT_FOREACH_CHILD_STATUS_OKAY(PORTS, MAY_DECLARE)
#define PIN_CONFIG(node) COND_CODE_1(DT_NODE_HAS_PROP(DT_PHANDLE(node, uart), pinctrl_0), \
	(PINCTRL_DT_DEV_CONFIG_GET(DT_PHANDLE(node, uart))), (NULL))
#else
#define PIN_CONFIG(node) NULL
#endif
static const struct {
	const struct device *uart;
	const struct pinctrl_dev_config *pins;
} config[BP_PORT_COUNT] = {
#define UART_CONFIG(node) COND_CODE_1(DT_NODE_HAS_PROP(node, uart), \
	([DT_REG_ADDR(node)] = {DEVICE_DT_GET(DT_PHANDLE(node, uart)), PIN_CONFIG(node)},), ())
	DT_FOREACH_CHILD_STATUS_OKAY(PORTS, UART_CONFIG)
};

enum phase { OFFER, OFFER_TX, OFFER_ACK, SYNC, ACK_TX, BAUD_WAIT, DATA };
static struct link {
	struct lump lump;
	struct port_sensor sensor;
	struct ring_buf rx;
	uint8_t rx_buffer[RX_CAPACITY], tx[34];
	size_t tx_size, tx_pos;
	int error;
	bool active, tx_busy;
	int64_t tx_done_ms, tx_deadline, born_ms, deadline, keepalive_ms, data_ms;
	uint32_t baud;
	uint32_t measurement;
	bool selecting;
	enum phase phase;
} links[BP_PORT_COUNT];
/* The parser owns the sole metadata copy. Publish only after negotiation;
 * invalidate under this lock before resetting it. No UART I/O holds the lock. */
static struct k_spinlock metadata_lock;
static bool metadata_ready[BP_PORT_COUNT];

int port_uart_get_mode(uint32_t port, uint32_t mode, struct bp_port_mode *value)
{
	k_spinlock_key_t key = k_spin_lock(&metadata_lock);
	int err = !metadata_ready[port] ? -EAGAIN : mode >= links[port].lump.num_modes ? -EINVAL : 0;
	if (!err) *value = links[port].lump.modes[mode];
	k_spin_unlock(&metadata_lock, key);
	return err;
}

static void disable_irq(const struct device *dev)
{
	uart_irq_rx_disable(dev);
	uart_irq_tx_disable(dev);
	uart_irq_err_disable(dev);
}

static void interrupt(const struct device *dev, void *arg)
{
	struct link *l = arg;
	if (!l->active) {
		return;
	}
	int err = uart_irq_update(dev);
	if (err < 0) {
		goto failed;
	}
	if (!err) {
		return;
	}
	err = uart_err_check(dev);
	if (err) {
		/* A Powered Up peer can signal BREAK before TYPE after acknowledging
		 * the fast baud. Synchronize on a valid TYPE, as PBIO does; once TYPE
		 * is known, any receive error invalidates the session. */
		if (err < 0 || (err & ~(UART_ERROR_FRAMING | UART_BREAK)) || l->lump.phase != LUMP_TYPE) {
			err = err < 0 ? err : -EIO;
			goto failed;
		}
	}
	uint8_t bytes[32];
	err = uart_irq_rx_ready(dev);
	if (err < 0) {
		goto failed;
	}
	if (err) {
		/* Drain edge-triggered FIFOs until a short read. The worker cannot
		 * consume this ring during the ISR, so overflow also bounds a flood. */
		int count;
		do {
			count = uart_fifo_read(dev, bytes, sizeof(bytes));
			if (count < 0) {
				err = count;
				goto failed;
			}
			if (ring_buf_put(&l->rx, bytes, count) != count) {
				err = -ENOBUFS;
				goto failed;
			}
		} while (count == sizeof(bytes));
	}
	err = l->tx_busy ? uart_irq_tx_ready(dev) : 0;
	if (err < 0) {
		goto failed;
	}
	if (err) {
		if (l->tx_pos < l->tx_size) {
			int count = uart_fifo_fill(dev, l->tx + l->tx_pos, l->tx_size - l->tx_pos);
			if (count < 0) {
				err = count;
				goto failed;
			}
			l->tx_pos += count;
		}
		/* STM32 uses TC interrupts: completion includes the last stop bit,
		 * not just an empty software queue or writable data register. */
		err = l->tx_pos == l->tx_size ? uart_irq_tx_complete(dev) : 0;
		if (err < 0) {
			goto failed;
		}
		if (err) {
			uart_irq_tx_disable(dev);
			l->tx_done_ms = k_uptime_get();
			l->tx_busy = false;
		}
	}
	return;
failed:
	l->error = err;
	l->active = false;
	disable_irq(dev);
}

bool port_uart_available(uint32_t port)
{
	return port < BP_PORT_COUNT && config[port].uart;
}

void port_uart_stop(uint32_t port)
{
	if (port >= BP_PORT_COUNT) return;
	k_spinlock_key_t snapshot_key = k_spin_lock(&metadata_lock);
	metadata_ready[port] = false;
	k_spin_unlock(&metadata_lock, snapshot_key);
	if (!port_uart_available(port) || !device_is_ready(config[port].uart)) {
		return;
	}
	unsigned int key = irq_lock();
	links[port].active = false;
	disable_irq(config[port].uart);
	irq_unlock(key);
}

static int set_baud(uint32_t port, uint32_t baud)
{
	const struct device *dev = config[port].uart;
	struct link *l = &links[port];
	port_uart_stop(port);
	struct uart_config cfg = {.baudrate = baud, .parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1, .data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE};
	int err = uart_configure(dev, &cfg);
	if (err) {
		return err;
	}
	/* Drop bytes from the previous baud and clear its expected framing error.
	 * With F4's single-byte receive register this loop normally drains <=1. */
	uint8_t byte;
	for (size_t i = 0; ; i++) {
		err = uart_poll_in(dev, &byte);
		if (err == -1) {
			break;
		}
		if (err || i == RX_CAPACITY) {
			return err ? err : -EOVERFLOW;
		}
	}
	err = uart_err_check(dev);
	if (err < 0) {
		return err;
	}
	ring_buf_reset(&l->rx);
	l->baud = baud;
	l->active = true;
	uart_irq_err_enable(dev);
	uart_irq_rx_enable(dev);
	return 0;
}

int port_uart_start(uint32_t port)
{
	if (!port_uart_available(port) || !device_is_ready(config[port].uart)) {
		return -ENODEV;
	}
	port_uart_stop(port);
	struct link *l = &links[port];
	*l = (struct link){0};
	lump_reset(&l->lump);
	l->measurement = UINT32_MAX;
	l->sensor.value.error = -EAGAIN;
	ring_buf_init(&l->rx, sizeof(l->rx_buffer), l->rx_buffer);
	int err = uart_irq_callback_user_data_set(config[port].uart, interrupt, l);
#if defined(CONFIG_PINCTRL)
	if (!err && config[port].pins) {
		err = pinctrl_apply_state(config[port].pins, PINCTRL_STATE_DEFAULT);
	}
#endif
	if (!err) {
		err = set_baud(port, 115200);
	}
	l->born_ms = k_uptime_get();
	return err;
}

static int transmit(uint32_t port, const uint8_t *bytes, size_t count)
{
	struct link *l = &links[port];
	unsigned int key = irq_lock();
	int err = l->error ? l->error : l->tx_busy ? -EBUSY : 0;
	if (!err) {
		memcpy(l->tx, bytes, count);
		l->tx_pos = 0;
		l->tx_size = count;
		l->tx_busy = true;
		l->tx_deadline = k_uptime_get() + IO_TIMEOUT_MS;
		uart_irq_tx_enable(config[port].uart);
	}
	irq_unlock(key);
	return err;
}

static int receive(uint32_t port, int64_t now)
{
	struct link *l = &links[port];
	for (size_t n = 0; n < RX_BUDGET; n++) {
		uint8_t byte;
		unsigned int key = irq_lock();
		bool have = ring_buf_get(&l->rx, &byte, 1) == 1;
		irq_unlock(key);
		if (!have) {
			break;
		}
		if (l->phase == OFFER_ACK) {
			int err = byte == LUMP_SYS_ACK ? 0 : set_baud(port, 2400);
			l->phase = SYNC;
			l->deadline = now + IO_TIMEOUT_MS;
			if (err) {
				return err;
			}
			continue;
		}
		bool beginning = !l->lump.used;
		int result = lump_receive(&l->lump, byte);
		if (result < 0) {
			return result;
		}
		if (result == 1) {
			l->data_ms = now;
			port_sensor_receive(&l->sensor, &l->lump, now);
		}
		if (l->lump.phase != LUMP_TYPE && (beginning || !l->lump.used)) {
			l->deadline = now + IO_TIMEOUT_MS;
		}
		if (l->lump.phase == LUMP_ACK) {
			const uint8_t ack = LUMP_SYS_ACK;
			l->phase = ACK_TX;
			return transmit(port, &ack, 1);
		}
	}
	return 0;
}

int port_uart_poll(uint32_t port, struct bp_port_state *out,
	struct bp_sensor_sample *sensor, uint32_t measurement)
{
	struct link *l = &links[port];
	int64_t now = k_uptime_get();
	unsigned int key = irq_lock();
	int err = l->error;
	bool tx_busy = l->tx_busy;
	int64_t tx_done = l->tx_done_ms;
	irq_unlock(key);
	if (err || (tx_busy && now >= l->tx_deadline) ||
	    (l->phase != DATA && now - l->born_ms >= SYNC_TIMEOUT_MS)) {
		return err ? err : -ETIMEDOUT;
	}
	if (l->phase == OFFER) {
		uint8_t payload[4], command[34];
		sys_put_le32(115200, payload);
		int size = lump_command(command, LUMP_CMD_SPEED, payload, sizeof(payload));
		err = transmit(port, command, size);
		l->phase = OFFER_TX;
	} else if (l->phase == OFFER_TX && !tx_busy) {
		l->deadline = tx_done + 10;
		l->phase = OFFER_ACK;
	} else if (l->phase == ACK_TX && !tx_busy) {
		l->deadline = tx_done + 10;
		l->phase = BAUD_WAIT;
	} else if (l->phase == BAUD_WAIT && now >= l->deadline) {
		err = set_baud(port, l->lump.baud);
		l->lump.phase = LUMP_DATA;
		l->phase = DATA;
		l->keepalive_ms = now;
		l->deadline = now + IO_TIMEOUT_MS;
		/* Until the first DATA, allow the same three keep-alive intervals. */
		l->data_ms = now;
		port_sensor_reset(&l->sensor, &l->lump);
		k_spinlock_key_t snapshot_key = k_spin_lock(&metadata_lock);
		metadata_ready[port] = !err;
		k_spin_unlock(&metadata_lock, snapshot_key);
	}
	if (l->phase == DATA && !err) {
		if (l->selecting && !tx_busy) {
			l->selecting = false;
			port_sensor_sent(&l->sensor, tx_done);
		}
		if (l->measurement != measurement && !l->selecting) {
			l->measurement = measurement;
			int result = port_sensor_request(&l->sensor, &l->lump, measurement);
			if (result) {
				l->sensor.pending = l->sensor.awaiting = false;
				l->sensor.mode = UINT8_MAX;
				l->sensor.value.error = result;
				l->sensor.value.fields = 0;
			}
		}
	}
	if (!err && (l->phase == OFFER_ACK || l->phase == SYNC || l->phase == DATA)) {
		err = receive(port, now);
	}
	if (!err && l->phase == OFFER_ACK && now >= l->deadline) {
		err = set_baud(port, 2400);
		l->phase = SYNC;
		l->deadline = now + IO_TIMEOUT_MS;
	}
	if (!err && (l->phase == SYNC || (l->phase == DATA && l->lump.used)) && now >= l->deadline) {
		err = -ETIMEDOUT;
	}
	if (!err && l->phase == DATA) {
		if (!l->sensor.pending && (l->sensor.awaiting || l->sensor.first_ms >= 0) &&
		    l->sensor.mode != UINT8_MAX && now >= l->sensor.deadline) {
			return -ETIMEDOUT;
		}
		if (now - l->data_ms >= 3 * KEEPALIVE_MS) {
			return -ETIMEDOUT;
		}
		if (now >= l->keepalive_ms && !l->tx_busy) {
			const uint8_t nack = LUMP_SYS_NACK;
			err = transmit(port, &nack, 1);
			l->keepalive_ms = now + KEEPALIVE_MS;
		} else if (l->sensor.pending && !l->selecting && !l->tx_busy) {
			uint8_t command[34];
			int size = lump_command(command, LUMP_CMD_SELECT, &l->sensor.mode, 1);
			err = transmit(port, command, size);
			l->selecting = !err;
		}
	}
	if (err) {
		return err;
	}
	out->phase = l->phase == DATA ? BP_PORT_READY : BP_PORT_NEGOTIATING;
	out->error = l->lump.length ? 0 : -EAGAIN;
	out->type_id = l->lump.phase == LUMP_TYPE ? 14 : l->lump.id;
	out->baudrate = l->baud;
	out->num_modes = l->lump.num_modes;
	out->num_view_modes = l->lump.num_view_modes;
	out->has_version = l->lump.got_version;
	out->firmware_version = l->lump.firmware_version;
	out->hardware_version = l->lump.hardware_version;
	out->capabilities = l->lump.capabilities;
	out->bad_frames = l->lump.bad_frames;
	out->length = l->lump.length;
	out->data_sequence = l->lump.sequence;
	out->data_ms = l->lump.length ? l->data_ms : 0;
	out->mode = l->lump.mode;
	out->data_type = l->lump.modes[l->lump.mode].data_type;
	out->num_values = l->lump.modes[l->lump.mode].num_values;
	memcpy(out->data, l->lump.data, out->length);
	*sensor = l->sensor.value;
	sensor->type_id = out->type_id;
	if (l->measurement != measurement) {
		sensor->error = -EAGAIN;
		sensor->fields = 0;
	}
	/* Encoder modes never authorize power across the motor terminals,
	 * regardless of advertised supply flags. Logic power is separate. */
	if (l->phase == DATA && l->sensor.mode != UINT8_MAX &&
	    l->sensor.value.measurement != BP_SENSOR_ANGLE) {
		uint8_t supply = l->lump.capabilities & (LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN1 |
			LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN2);
		if (supply && ((l->lump.capabilities & (LUMP_MODE_FLAGS0_MOTOR_SPEED |
		    LUMP_MODE_FLAGS0_MOTOR_ABS_POS | LUMP_MODE_FLAGS0_MOTOR_REL_POS |
		    LUMP_MODE_FLAGS0_MOTOR_POWER | LUMP_MODE_FLAGS0_MOTOR)) ||
		    supply == (LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN1 | LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN2))) {
			return -EPROTO;
		}
		out->supply = supply == LUMP_MODE_FLAGS0_NEEDS_SUPPLY_PIN1 ? 1 : supply ? 2 : 0;
	}
	return 0;
}
