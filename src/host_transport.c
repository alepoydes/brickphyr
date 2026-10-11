// SPDX-License-Identifier: Apache-2.0
#include "host_protocol.h"
#include "host_transport.h"
#include "hub_power.h"
#include "hub_matrix.h"
#include "hub_sound.h"
#include "hub_ports.h"
#include "storage.h"
#include "native.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp_shell.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/byteorder.h>

static const struct device *uart;
static uint16_t variant;
RING_BUF_DECLARE(receive_buffer, 256);
RING_BUF_DECLARE(transmit_buffer, 256);
K_SEM_DEFINE(wake, 0, 1);
K_SEM_DEFINE(shell_closed, 0, 1);
K_SEM_DEFINE(smp_tx_ready, 0, 1);
/* Each complete line is consumed synchronously before reading the next one. */
NET_BUF_POOL_DEFINE(smp_lines, 1, SMP_SHELL_RX_BUF_SIZE, 0, NULL);
static struct smp_shell_data smp_input = { .buf_pool = &smp_lines };
static struct mcumgr_serial_rx_ctxt smp_serial;
static struct smp_transport smp_transport;
static struct k_spinlock lock;
static uint8_t mode;
static int next_mode = -1;
static bool occupied;
static bool usb_open;
static bool ble_occupied;
static bool ble_closing;
static bool ble_exit;
/* The single stream/Shell can belong to USB or to the native BLE interface. */
static bool ble_shell;
static void (*ble_tx_ready)(void);
static void finish_ble_session(void);

static uint8_t stream_mode(void)
{
	return ble_shell ? HOST_SHELL : mode;
}
static k_tid_t ble_request_thread;
static bool ready;
static bool tx_idle;
static int first_error;
static int shell_close_error;
static bool running;
static uint32_t generation;
static uint32_t shell_generation;
static shell_transport_handler_t shell_handler;
static void *shell_context;

static bool shell_owner(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool valid = occupied && running && stream_mode() == HOST_SHELL &&
		generation == shell_generation;
	k_spin_unlock(&lock, key);
	return valid;
}

static void process_smp(void)
{
	struct net_buf *packet;

	while ((packet = smp_shell_rx_packet(&smp_input, &smp_serial)) != NULL) {
		if (!shell_owner()) {
			smp_packet_free(packet);
			continue;
		}
		struct cbor_nb_reader reader;
		struct cbor_nb_writer writer;
		struct smp_streamer stream = {
			.smpt = &smp_transport, .reader = &reader, .writer = &writer,
		};
		/* The public SMP processor consumes packet. Running on the Shell thread
		 * serializes output and lets teardown join any accepted operation. */
		smp_process_request_packet(&stream, packet);
	}
}

static enum mgmt_cb_return management_access(uint32_t event, enum mgmt_cb_return previous,
	int32_t *rc, uint16_t *group, bool *abort_more, void *data, size_t size)
{
	if (previous != MGMT_CB_OK) {
		return previous;
	}
	bool ble_allowed = event == MGMT_EVT_OP_CMD_RECV ?
		host_ble_command_allowed() : host_ble_request_active();
	if (!shell_owner() && !ble_allowed) {
		*rc = MGMT_ERR_EACCESSDENIED;
		return MGMT_CB_ERROR_RC;
	}
	if (event == MGMT_EVT_OP_OS_MGMT_RESET && (native_shutdown() || hub_sound_shutdown())) {
		*rc = MGMT_ERR_EUNKNOWN;
		return MGMT_CB_ERROR_RC;
	}
	if (event == MGMT_EVT_OP_SETTINGS_MGMT_ACCESS) {
		const struct settings_mgmt_access *access = data;
		/* Whole-store save persists RAM, as in Shell. Delete bypasses runtime
		 * defaults; bulk load currently hides backend errors in Zephyr. */
		if (access->access == SETTINGS_ACCESS_DELETE || access->access == SETTINGS_ACCESS_LOAD ||
		    (access->access == SETTINGS_ACCESS_SAVE &&
		     (access->name || !IS_ENABLED(CONFIG_SETTINGS_FILE)))) {
			*rc = MGMT_ERR_ENOTSUP;
			return MGMT_CB_ERROR_RC;
		}
		if (access->access == SETTINGS_ACCESS_SAVE && IS_ENABLED(CONFIG_SETTINGS_FILE) &&
		    storage_status()) {
			*rc = MGMT_ERR_EUNKNOWN;
			return MGMT_CB_ERROR_RC;
		}
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback command_access = {
	.callback = management_access, .event_id = MGMT_EVT_OP_CMD_RECV,
};
static struct mgmt_callback settings_access = {
	.callback = management_access, .event_id = MGMT_EVT_OP_SETTINGS_MGMT_ACCESS,
};
#ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
static struct mgmt_callback reset_access = {
	.callback = management_access, .event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};
#endif

static void notify_shell(enum shell_transport_evt event)
{
	if (running && occupied && stream_mode() == HOST_SHELL) {
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
		bool usb_stream = occupied && !ble_shell;
		size_t space = usb_stream ? ring_buf_space_get(&receive_buffer) : sizeof(data);

		while (space && (length = uart_fifo_read(dev, data, MIN(space, sizeof(data)))) > 0) {
			if (usb_stream) {
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
		uint32_t length = ble_shell ? 0 : ring_buf_get_claim(&transmit_buffer, &data, 64);
		int sent = length ? uart_fifo_fill(dev, data, length) : 0;
		if (!ble_shell) {
			ring_buf_get_finish(&transmit_buffer, sent);
		}
		if (ble_shell || ring_buf_is_empty(&transmit_buffer)) {
			uart_irq_tx_disable(dev);
		}
		notify_shell(SHELL_TRANSPORT_EVT_TX_RDY);
	}
	int complete = uart_irq_tx_complete(dev);
	if (complete < 0 && !first_error) {
		first_error = complete;
	}
	tx_idle = complete > 0 && (ble_shell || ring_buf_is_empty(&transmit_buffer));
	k_spin_unlock(&lock, key);
	k_sem_give(&wake);
	k_sem_give(&smp_tx_ready);
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
	*count = 0;
	while (*count < length) {
		uint8_t byte;
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool received = occupied && generation == shell_generation &&
			ring_buf_get(&receive_buffer, &byte, 1);
		k_spin_unlock(&lock, key);
		if (!received) {
			break;
		}
		if (!smp_shell_rx_bytes(&smp_input, &byte, 1)) {
			((uint8_t *)data)[(*count)++] = byte;
		} else if (byte == '\n') {
			process_smp();
		}
	}
	uart_irq_rx_enable(uart);
	return 0;
}

/* Called under lock; the BLE callback only schedules nonblocking output work. */
static void output_ready(void)
{
	if (ble_shell) {
		ble_tx_ready();
	} else {
		uart_irq_tx_enable(uart);
	}
}

static int transport_write(const struct shell_transport *transport, const void *data,
			   size_t length, size_t *count)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	/* Complete discarded writes on disconnect so a command cannot block teardown. */
	if (occupied && generation == shell_generation) {
		*count = ring_buf_put(&transmit_buffer, data, length);
		if (!ble_shell) {
			tx_idle = false;
		}
		output_ready();
	} else {
		*count = length;
	}
	k_spin_unlock(&lock, key);
	return 0;
}

static int smp_write(const void *data, int length)
{
	const uint8_t *bytes = data;
	while (length) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool valid = occupied && generation == shell_generation;
		size_t sent = valid ? ring_buf_put(&transmit_buffer, bytes, length) : 0;
		if (sent) {
			if (!ble_shell) {
				tx_idle = false;
			}
			output_ready();
		}
		k_spin_unlock(&lock, key);
		if (!valid) {
			return -ENOTCONN;
		}
		bytes += sent;
		length -= sent;
		if (!sent) {
			k_sem_take(&smp_tx_ready, K_FOREVER);
		}
	}
	return 0;
}

static int smp_output(struct net_buf *packet)
{
	int result = mcumgr_serial_tx_pkt(packet->data, packet->len, smp_write);
	smp_packet_free(packet);
	return result ? MGMT_ERR_EUNKNOWN : MGMT_ERR_EOK;
}

static uint16_t smp_mtu(const struct net_buf *packet)
{
	/* The serial codec fragments a complete SMP packet into 127-byte lines.
	 * SMP itself must not split it into separately framed partial messages. */
	return CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
}

static const struct shell_transport_api transport_api = {
	.init = transport_init,
	.uninit = transport_uninit,
	.enable = transport_enable,
	.read = transport_read,
	.write = transport_write,
};
static const struct shell_transport transport = { .api = &transport_api };
SHELL_DEFINE(host_shell, "brickphyr> ", &transport, 0, 0, SHELL_FLAG_OLF_CRLF);

static int cmd_host_mode(const struct shell *shell, size_t argc, char **argv)
{
	if (strcmp(argv[1], "pybricks")) {
		shell_error(shell, "Usage: host_mode pybricks");
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool owner = occupied && running && stream_mode() == HOST_SHELL &&
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
		uint8_t selected = stream_mode();
		uint32_t current_generation = generation;
		if (changed) {
			running = false;
		}
		k_spin_unlock(&lock, key);
		if (changed) {
			if (shell_initialized) {
				shell_uninit(&host_shell, shell_uninitialized);
				k_sem_take(&shell_closed, K_FOREVER);
				k_thread_join(host_shell.thread, K_FOREVER);
				if (shell_close_error) {
					key = k_spin_lock(&lock);
					if (!first_error) {
						first_error = shell_close_error;
					}
					k_spin_unlock(&lock, key);
				}
				shell_initialized = false;
			}
			native_session_closed();
			hub_matrix_session_closed();
			hub_sound_session_closed();
			hub_sensors_release(BP_SENSOR_TERMINAL);
			smp_shell_rx_reset(&smp_input, &smp_serial);
			protocol = (struct host_protocol){0};
			reply_length = 0;
			session_generation = current_generation;
		}
		if (!open) {
			key = k_spin_lock(&lock);
			if (generation == session_generation) {
				ready = true;
				ble_shell = false;
				ble_tx_ready = NULL;
				finish_ble_session();
				if (!ble_occupied) {
					hub_power_connection(false);
				}
			}
			k_spin_unlock(&lock, key);
			continue;
		}
		if (selected == HOST_SHELL && !shell_initialized) {
			static const struct shell_backend_config_flags flags = SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;
			shell_generation = session_generation;
			int err = shell_init(&host_shell, NULL, flags, false, 0);
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
	k_fifo_init(&smp_input.buf_ready);
	smp_transport.functions.output = smp_output;
	smp_transport.functions.get_mtu = smp_mtu;
	int err = uart_irq_callback_user_data_set(uart, uart_callback, NULL);
	if (err) {
		return err;
	}
	mgmt_callback_register(&command_access);
	mgmt_callback_register(&settings_access);
#ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
	mgmt_callback_register(&reset_access);
#endif
	k_thread_create(&host_worker, host_stack, K_THREAD_STACK_SIZEOF(host_stack),
			host_thread, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&host_worker, "host_stream");
	uart_irq_rx_enable(uart);
	/* Establish initial completion in the UART callback without requiring
	 * a USB status request before the first Bluetooth connection. */
	uart_irq_tx_enable(uart);
	k_sem_give(&wake);
	return 0;
}

/* Caller holds lock. Accepted commands finish; queued bytes and old replies die. */
static void stream_changed(bool open)
{
	occupied = open;
	ready = false;
	generation++;
	ring_buf_reset(&receive_buffer);
	ring_buf_reset(&transmit_buffer);
	if (running && stream_mode() == HOST_SHELL) {
		shell_handler(SHELL_TRANSPORT_EVT_TX_RDY, shell_context);
	}
	if (!open && next_mode >= 0) {
		mode = next_mode;
		next_mode = -1;
	}
	k_sem_give(&wake);
	k_sem_give(&smp_tx_ready);
}

void host_connection_changed(bool open)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (open && usb_open) {
		k_spin_unlock(&lock, key);
		return;
	}
	if (open) {
		/* DTR is delivered before this port's new input. Drain the driver's
		 * FIFO synchronously: its deferred IRQ callback may still be pending.
		 * poll_in is the public nonblocking API outside that callback; the
		 * same lock excludes our IRQ reader until admission is complete. */
		unsigned char stale;
		int err;
		while ((err = uart_poll_in(uart, &stale)) == 0) {
		}
		if (err != -1 && !first_error) {
			first_error = err;
		}
	}
	usb_open = open;
	if (!ble_occupied && open != occupied &&
	    (!open || (!first_error && hub_power_connection(true)))) {
		stream_changed(open);
	}
	k_spin_unlock(&lock, key);
	uart_irq_rx_enable(uart);
}

void host_interface_status(uint8_t state[8])
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	state[0] = 1;
	state[1] = mode;
	state[2] = occupied || ble_occupied;
	state[3] = ready && tx_idle && !ble_occupied && !first_error;
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
	if (usb_open || occupied || ble_occupied || !ready || !tx_idle || first_error) {
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

int host_ble_acquire(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = 0;
	/* Old USB output still gates a USB protocol switch, not a BLE session. */
	if (usb_open || occupied || ble_occupied || !ready || first_error) {
		err = -EBUSY;
	} else if (!hub_power_connection(true)) {
		err = -ESHUTDOWN;
	} else {
		ble_occupied = true;
	}
	k_spin_unlock(&lock, key);
	return err;
}

static void finish_ble_session(void)
{
	if (ble_closing && !ble_request_thread && !ble_shell) {
		native_session_closed();
		hub_matrix_session_closed();
		hub_sound_session_closed();
		hub_sensors_release(BP_SENSOR_TERMINAL);
		ble_occupied = false;
		ble_closing = false;
		hub_power_connection(false);
		if (ble_exit) {
			mode = HOST_SHELL;
		}
		ble_exit = false;
	}
}

void host_ble_release(bool exit_compatibility)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (ble_occupied && !ble_closing) {
		ble_closing = true;
		ble_exit = exit_compatibility;
		if (ble_shell && occupied) {
			stream_changed(false);
		}
		finish_ble_session();
	}
	/* A rejected USB port must close and reopen; never inherit its input. */
	k_spin_unlock(&lock, key);
}

bool host_ble_request_begin(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool valid = ble_occupied && !ble_closing && !ble_request_thread && !ble_shell;
	if (valid) {
		ble_request_thread = k_current_get();
	}
	k_spin_unlock(&lock, key);
	return valid;
}

void host_ble_request_end(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	__ASSERT_NO_MSG(ble_request_thread == k_current_get());
	ble_request_thread = NULL;
	finish_ble_session();
	k_spin_unlock(&lock, key);
}

bool host_ble_request_active(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool active = ble_request_thread == k_current_get();
	k_spin_unlock(&lock, key);
	return active;
}

uint16_t host_hub_variant(void)
{
	return variant;
}

bool host_ble_command_allowed(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool allowed = ble_request_thread == k_current_get() && !ble_closing;
	k_spin_unlock(&lock, key);
	return allowed;
}

int host_ble_shell_open(void (*tx_ready)(void), uint32_t *session)
{
	if (!tx_ready || !session) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = 0;
	if (!ble_occupied || ble_closing) {
		err = -ENOTCONN;
	} else if (ble_shell || ble_request_thread || !ready || first_error) {
		err = -EBUSY;
	} else {
		ble_shell = true;
		ble_tx_ready = tx_ready;
		stream_changed(true);
		*session = generation;
	}
	k_spin_unlock(&lock, key);
	return err;
}

static bool ble_stream_valid(uint32_t session)
{
	return ble_shell && occupied && generation == session;
}

void host_ble_shell_close(uint32_t session)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (ble_stream_valid(session)) {
		stream_changed(false);
	}
	k_spin_unlock(&lock, key);
}

int host_ble_shell_receive(uint32_t session, const void *data, size_t length)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = 0;
	if (!ble_stream_valid(session)) {
		err = -ENOTCONN;
	} else if (ring_buf_space_get(&receive_buffer) < length) {
		err = -ENOBUFS;
	} else {
		ring_buf_put(&receive_buffer, data, length);
		notify_shell(SHELL_TRANSPORT_EVT_RX_RDY);
	}
	k_spin_unlock(&lock, key);
	return err;
}

size_t host_ble_shell_peek(uint32_t session, void *data, size_t capacity)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	size_t length = ble_stream_valid(session) ?
		ring_buf_peek(&transmit_buffer, data, capacity) : 0;
	k_spin_unlock(&lock, key);
	return length;
}

void host_ble_shell_consume(uint32_t session, size_t length)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (ble_stream_valid(session)) {
		ring_buf_get(&transmit_buffer, NULL, length);
		notify_shell(SHELL_TRANSPORT_EVT_TX_RDY);
		k_sem_give(&smp_tx_ready);
	}
	k_spin_unlock(&lock, key);
}
