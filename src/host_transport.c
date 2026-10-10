// SPDX-License-Identifier: Apache-2.0
#include "host_protocol.h"
#include "host_transport.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/byteorder.h>

static const struct device *uart;
static uint16_t variant;
RING_BUF_DECLARE(receive_buffer, 256);
RING_BUF_DECLARE(transmit_buffer, 256);
K_SEM_DEFINE(wake, 0, 1);
K_SEM_DEFINE(shell_closed, 0, 1);
static struct k_spinlock lock;
static uint8_t mode;
static int next_mode = -1;
static bool occupied;
static bool ready;
static bool tx_idle;
static int first_error;
static int shell_close_error;
static bool running;
static uint32_t generation;
static uint32_t shell_generation;
static shell_transport_handler_t shell_handler;
static void *shell_context;

static void notify_shell(enum shell_transport_evt event)
{
	if (running && occupied && mode == HOST_SHELL) {
		shell_handler(event, shell_context);
	}
}

static void uart_callback(const struct device *dev, void *context)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (!uart_irq_update(dev)) {
		k_spin_unlock(&lock, key);
		return;
	}
	if (uart_irq_rx_ready(dev)) {
		uint8_t data[64];
		int length;
		size_t space = occupied ? ring_buf_space_get(&receive_buffer) : sizeof(data);

		while (space && (length = uart_fifo_read(dev, data, MIN(space, sizeof(data)))) > 0) {
			if (occupied) {
				ring_buf_put(&receive_buffer, data, length);
				space = ring_buf_space_get(&receive_buffer);
			}
		}
		if (!space) {
			uart_irq_rx_disable(dev);
		}
		notify_shell(SHELL_TRANSPORT_EVT_RX_RDY);
	}
	if (uart_irq_tx_ready(dev)) {
		uint8_t *data;
		uint32_t length = ring_buf_get_claim(&transmit_buffer, &data, 64);
		int sent = length ? uart_fifo_fill(dev, data, length) : 0;
		ring_buf_get_finish(&transmit_buffer, sent);
		if (ring_buf_is_empty(&transmit_buffer)) {
			uart_irq_tx_disable(dev);
		}
		notify_shell(SHELL_TRANSPORT_EVT_TX_RDY);
	}
	int complete = uart_irq_tx_complete(dev);
	if (complete < 0 && !first_error) {
		first_error = complete;
	}
	tx_idle = complete > 0 && ring_buf_is_empty(&transmit_buffer);
	k_spin_unlock(&lock, key);
	k_sem_give(&wake);
}

static int transport_init(const struct shell_transport *transport, const void *config,
			  shell_transport_handler_t handler, void *context)
{
	shell_handler = handler;
	shell_context = context;
	return 0;
}

static int transport_uninit(const struct shell_transport *transport)
{
	return 0;
}

static int transport_enable(const struct shell_transport *transport, bool blocking)
{
	return blocking ? -ENOTSUP : 0;
}

static int transport_read(const struct shell_transport *transport, void *data,
			  size_t length, size_t *count)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	*count = occupied && generation == shell_generation ? ring_buf_get(&receive_buffer, data, length) : 0;
	k_spin_unlock(&lock, key);
	uart_irq_rx_enable(uart);
	return 0;
}

static int transport_write(const struct shell_transport *transport, const void *data,
			   size_t length, size_t *count)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	/* Complete discarded writes on disconnect so a command cannot block teardown. */
	if (occupied && generation == shell_generation) {
		*count = ring_buf_put(&transmit_buffer, data, length);
		tx_idle = false;
	} else {
		*count = length;
	}
	k_spin_unlock(&lock, key);
	uart_irq_tx_enable(uart);
	return 0;
}

static const struct shell_transport_api transport_api = {
	.init = transport_init,
	.uninit = transport_uninit,
	.enable = transport_enable,
	.read = transport_read,
	.write = transport_write,
};
static const struct shell_transport transport = { .api = &transport_api };
SHELL_DEFINE(usb_shell, "brickphyr> ", &transport, 0, 0, SHELL_FLAG_OLF_CRLF);

static int cmd_host_mode(const struct shell *shell, size_t argc, char **argv)
{
	if (strcmp(argv[1], "pybricks")) {
		shell_error(shell, "Usage: host_mode pybricks");
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool owner = occupied && running && mode == HOST_SHELL &&
		generation == shell_generation;
	if (owner) {
		next_mode = HOST_PYBRICKS;
	}
	k_spin_unlock(&lock, key);
	if (!owner) {
		return -ENOTCONN;
	}
	shell_print(shell, "Pybricks selected for the next session. Close this port.");
	return 0;
}

SHELL_CMD_ARG_REGISTER(host_mode, NULL, "host_mode pybricks: switch when this port closes.",
		      cmd_host_mode, 2, 0);

static void shell_uninitialized(const struct shell *shell, int result)
{
	shell_close_error = result;
	k_sem_give(&shell_closed);
}

static void host_thread(void *a, void *b, void *c)
{
	struct host_protocol protocol = {0};
	uint8_t reply[HOST_PROTOCOL_FRAME_MAX];
	size_t reply_length = 0;
	uint32_t session_generation = 0;
	bool shell_initialized = false;

	for (;;) {
		k_sem_take(&wake, K_FOREVER);
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool changed = session_generation != generation;
		bool open = occupied;
		uint8_t selected = mode;
		uint32_t current_generation = generation;
		if (changed) {
			running = false;
		}
		k_spin_unlock(&lock, key);
		if (changed) {
			if (shell_initialized) {
				shell_uninit(&usb_shell, shell_uninitialized);
				k_sem_take(&shell_closed, K_FOREVER);
				k_thread_join(usb_shell.thread, K_FOREVER);
				if (shell_close_error) {
					key = k_spin_lock(&lock);
					if (!first_error) {
						first_error = shell_close_error;
					}
					k_spin_unlock(&lock, key);
				}
				shell_initialized = false;
			}
			protocol = (struct host_protocol){0};
			reply_length = 0;
			session_generation = current_generation;
		}
		if (!open) {
			key = k_spin_lock(&lock);
			if (generation == session_generation) {
				ready = true;
			}
			k_spin_unlock(&lock, key);
			continue;
		}
		if (selected == HOST_SHELL && !shell_initialized) {
			static const struct shell_backend_config_flags flags = SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;
			shell_generation = session_generation;
			int err = shell_init(&usb_shell, NULL, flags, false, 0);
			if (err) {
				key = k_spin_lock(&lock);
				if (!first_error) {
					first_error = err;
				}
				k_spin_unlock(&lock, key);
				continue;
			}
			shell_initialized = true;
		}
		key = k_spin_lock(&lock);
		if (generation != session_generation) {
			k_spin_unlock(&lock, key);
			k_sem_give(&wake);
			continue;
		}
		running = true;
		if (selected == HOST_SHELL) {
			notify_shell(SHELL_TRANSPORT_EVT_RX_RDY);
			k_spin_unlock(&lock, key);
			continue;
		}
		k_spin_unlock(&lock, key);
		/* Backpressure leaves complete replies intact and pauses input consumption. */
		for (;;) {
			key = k_spin_lock(&lock);
			if (generation != session_generation) {
				k_spin_unlock(&lock, key);
				break;
			}
			if (reply_length) {
				if (ring_buf_space_get(&transmit_buffer) < reply_length) {
					k_spin_unlock(&lock, key);
					break;
				}
				ring_buf_put(&transmit_buffer, reply, reply_length);
				tx_idle = false;
				reply_length = 0;
				uart_irq_tx_enable(uart);
			}
			uint8_t byte;
			bool received = ring_buf_get(&receive_buffer, &byte, 1);
			k_spin_unlock(&lock, key);
			if (!received) {
				break;
			}
			reply_length = host_protocol_receive(&protocol, byte, variant, reply);
			if (protocol.exit_compatibility) {
				key = k_spin_lock(&lock);
				if (generation == session_generation) {
					next_mode = HOST_SHELL;
				}
				k_spin_unlock(&lock, key);
				protocol.exit_compatibility = false;
			}
		}
		uart_irq_rx_enable(uart);
	}
}

K_THREAD_STACK_DEFINE(host_stack, 2048);
static struct k_thread host_worker;

int host_transport_init(const struct device *device, uint16_t hub_variant)
{
	uart = device;
	variant = hub_variant;
	if (!device_is_ready(uart)) {
		return -ENODEV;
	}
	int err = uart_irq_callback_user_data_set(uart, uart_callback, NULL);
	if (err) {
		return err;
	}
	k_thread_create(&host_worker, host_stack, K_THREAD_STACK_SIZEOF(host_stack),
			host_thread, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&host_worker, "host_usb");
	uart_irq_rx_enable(uart);
	k_sem_give(&wake);
	return 0;
}

void host_connection_changed(bool open)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (open != occupied) {
		occupied = open;
		ready = false;
		generation++;
		ring_buf_reset(&receive_buffer);
		ring_buf_reset(&transmit_buffer);
		/* Wake a shell blocked waiting for output space before uninitializing it. */
		if (running && mode == HOST_SHELL) {
			shell_handler(SHELL_TRANSPORT_EVT_TX_RDY, shell_context);
		}
		if (!open && next_mode >= 0) {
			mode = next_mode;
			next_mode = -1;
		}
	}
	k_spin_unlock(&lock, key);
	uart_irq_rx_enable(uart);
	k_sem_give(&wake);
}

void host_interface_status(uint8_t state[8])
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	state[0] = 1;
	state[1] = mode;
	state[2] = occupied;
	state[3] = ready && tx_idle && !first_error;
	sys_put_le32(first_error, state + 4);
	k_spin_unlock(&lock, key);
	/* Refresh completion in the UART callback, the API's required context. */
	uart_irq_tx_enable(uart);
}

int host_interface_select(uint16_t requested)
{
	if (requested > HOST_SHELL) {
		return -ENOTSUP;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	int result = 0;
	if (occupied || !ready || !tx_idle || first_error) {
		result = -EBUSY;
	} else {
		mode = requested;
	}
	k_spin_unlock(&lock, key);
	return result;
}

enum host_mode host_interface_current(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	enum host_mode current = mode;
	k_spin_unlock(&lock, key);
	return current;
}
