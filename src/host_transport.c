// SPDX-License-Identifier: Apache-2.0
#define __ZEPHYR_SUPERVISOR__
#include "host_protocol.h"
#include "host_transport.h"
#include "hub_power.h"
#include "hub_matrix.h"
#include "hub_sound.h"
#include "hub_ports.h"
#include "storage.h"
#include "application.h"
#include "capture.h"

#include <errno.h>
#include <string.h>
#include <pbio/protocol.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp_shell.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/byteorder.h>

static const struct device *uart;
static uint16_t variant;
RING_BUF_DECLARE(receive_buffer, 256);
RING_BUF_DECLARE(transmit_buffer, 256);
static void host_work_run(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(host_work, host_work_run);
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
static bool ble_managed;
static struct k_sem *ble_idle_wake;
/* The single stream/Shell can belong to USB or to the native BLE interface. */
static bool ble_shell;
static void (*ble_tx_ready)(void);
static void finish_ble_session(void);

static uint8_t stream_mode(void)
{
	return !IS_ENABLED(CONFIG_BRICKPHYR_HOST_UART) || ble_shell ? HOST_SHELL : mode;
}
static k_tid_t ble_request_thread;
static bool ready;
static bool tx_idle;
static int first_error;
static bool running;
static uint32_t generation;
static uint32_t shell_generation;
/* A connection that negotiated SMP keeps that framing until it closes. */
static bool managed_shell;
static const struct shell host_shell;
static shell_transport_handler_t shell_handler;
static void *shell_context;

static void wake_host(void)
{
	/* Stream admission/teardown and binary requests share one management queue.
	 * Shell commands retain their worker so reset can join an entered command. */
	k_work_reschedule_for_queue(smp_get_wq(), &host_work, K_NO_WAIT);
}

/* Caller holds lock. Old USB output gates a USB switch, not BLE admission. */
static bool ble_available(void)
{
	return !usb_open && !occupied && !ble_occupied && ready && !first_error;
}

static void notify_ble_available(void)
{
	if (ble_idle_wake && ble_available()) k_sem_give(ble_idle_wake);
}

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
		managed_shell = true;
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
	if (ble_allowed) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		ble_managed = true;
		k_spin_unlock(&lock, key);
	}
	if (!shell_owner() && !ble_allowed) {
		*rc = MGMT_ERR_EACCESSDENIED;
		return MGMT_CB_ERROR_RC;
	}
	if (event == MGMT_EVT_OP_OS_MGMT_RESET && (application_shutdown() || hub_sound_shutdown())) {
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

#ifdef CONFIG_BRICKPHYR_HOST_UART
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
		/* The driver accepts as much contiguous data as its FIFO can hold.
		 * A USB packet size is not the UART API's transfer limit. */
		uint32_t length = ble_shell ? 0 : ring_buf_get_claim(&transmit_buffer, &data,
			ring_buf_capacity_get(&transmit_buffer));
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
	/* The active Shell receives FIFO events above. Only session transitions
	 * and the binary dispatcher need the management queue as well. */
	bool dispatch = !running || !occupied || stream_mode() != HOST_SHELL ||
		generation != shell_generation;
	k_spin_unlock(&lock, key);
	if (dispatch) wake_host();
	k_sem_give(&smp_tx_ready);
}

#endif

static void serial_rx_resume(void)
{
#ifdef CONFIG_BRICKPHYR_HOST_UART
	if (uart) uart_irq_rx_enable(uart);
#endif
}

static void serial_tx_resume(void)
{
#ifdef CONFIG_BRICKPHYR_HOST_UART
	if (uart) uart_irq_tx_enable(uart);
#endif
}

/* Called with lock held so a deferred UART callback cannot compete. */
static int serial_drain(void)
{
#ifdef CONFIG_BRICKPHYR_HOST_UART
	if (uart) {
		unsigned char stale;
		int err;
		while ((err = uart_poll_in(uart, &stale)) == 0) { }
		return err;
	}
#endif
	return -1;
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

static size_t stream_read(void *data, size_t capacity)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	size_t count = occupied && generation == shell_generation ?
		ring_buf_get(&receive_buffer, data, capacity) : 0;
	k_spin_unlock(&lock, key);
	return count;
}

static int transport_read(const struct shell_transport *transport, void *data,
			  size_t length, size_t *count)
{
	*count = 0;
	/* Session admission already discards stale bytes. Shell startup also
	 * drains its backend; retain new-session input until Shell is active. */
	if (!shell_ready(&host_shell)) return 0;
	while (*count < length) {
		uint8_t byte;
		if (!stream_read(&byte, 1)) {
			break;
		}
		if (!smp_shell_rx_bytes(&smp_input, &byte, 1)) {
			((uint8_t *)data)[(*count)++] = byte;
		} else if (byte == '\n') {
			process_smp();
		}
	}
	serial_rx_resume();
	return 0;
}

/* UART drivers may invoke their callback synchronously when enabling TX.
 * Never enable it while holding lock. The BLE hook only schedules work. */
static void output_ready(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool serial = occupied && !ble_shell;
	if (occupied && ble_shell) {
		ble_tx_ready();
	}
	k_spin_unlock(&lock, key);
	if (serial) {
		serial_tx_resume();
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
	} else {
		*count = length;
	}
	k_spin_unlock(&lock, key);
	output_ready();
	return 0;
}

static int smp_write(const void *data, int length)
{
	const uint8_t *bytes = data;
	while (length) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool valid = occupied && generation == shell_generation;
		size_t sent = valid ? ring_buf_put(&transmit_buffer, bytes, length) : 0;
		/* The serial codec emits four-byte Base64 fragments. Only wake the
		 * driver when the ring fills; smp_output flushes the completed packet.
		 * Line boundaries need no separate transfer on the shared byte stream. */
		bool flush = sent && !ring_buf_space_get(&transmit_buffer);
		if (sent) {
			if (!ble_shell) {
				tx_idle = false;
			}
		}
		k_spin_unlock(&lock, key);
		if (flush) {
			output_ready();
		}
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
	if (!result) output_ready();
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

#ifdef CONFIG_BRICKPHYR_PYTHON
static int cmd_python(const struct shell *shell, size_t argc, char **argv)
{
	if (shell != &host_shell || !shell_owner()) {
		return -ENOTCONN;
	}
	k_spinlock_key_t access_key = k_spin_lock(&lock);
	bool managed = managed_shell || (ble_shell && ble_managed);
	k_spin_unlock(&lock, access_key);
	if (managed) {
		shell_error(shell, "SMP is active. Use brickphyrdev terminal --repl for Python.");
		return -EBUSY;
	}
	int err = application_repl();
	if (err) {
		shell_error(shell, "REPL: %d", err);
		return err;
	}
	application_output_stream(true);
	shell_print(shell, "Ctrl-X: stop and return to Shell. Ctrl-D: reset Python.");
	uint8_t input[128], output[128];
	size_t received = 0, pending = 0, sent = 0;
	struct application_status status;
	while (shell_owner()) {
		application_get_status(&status);
		if (!status.repl) {
			break;
		}
		bool ended = status.state != APPLICATION_RUNNING;
		if (!ended && !received) {
			received = stream_read(input, sizeof(input));
			serial_rx_resume();
		}
		if (memchr(input, 0x18, received)) {
			err = application_stop();
			break;
		}
		if (!ended && received) {
			err = application_input(input, received);
			if (!err) {
				received = 0;
			} else if (err != -EAGAIN) {
				break;
			}
		}
		if (sent == pending) {
			pending = application_output(output, sizeof(output));
			sent = 0;
		}
		if (ended && !pending) {
			break;
		}
		if (pending > sent) {
			size_t count;
			transport_write(&transport, output + sent, pending - sent, &count);
			sent += count;
		}
		/* Two 20 ms legs leave a 50 ms human-interaction budget for keyboard
		 * delivery and echo. One bounded I/O batch per wake also yields to the
		 * lower-priority VM. Transport backpressure retains unsent bytes. */
		k_msleep(20);
	}
	application_output_stream(false);
	/* Disconnect must return promptly so teardown can reset the Shell. It
	 * neither stops Python nor transfers old bytes into the next session. */
	if (!shell_owner()) {
		return 0;
	}
	/* Unsent REPL keystrokes must not become Shell commands on exit. */
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (occupied && generation == shell_generation) {
		ring_buf_reset(&receive_buffer);
		if (!ble_shell) {
			(void)serial_drain();
		}
	}
	k_spin_unlock(&lock, key);
	serial_rx_resume();
	application_get_status(&status);
	shell_print(shell, "\nREPL: result %d; fault %u; cleanup %d; output dropped %u",
		status.result, status.fault, status.cleanup_error, status.output_dropped);
	if (err == -EAGAIN) {
		err = 0;
	}
	return err ? err : status.cleanup_error;
}
SHELL_CMD_ARG_REGISTER(python, NULL, "Enter Python REPL on a plain terminal.", cmd_python, 1, 0);
#endif

static int cmd_host_mode(const struct shell *shell, size_t argc, char **argv)
{
	if (!uart) return -ENOTSUP;
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

static void host_work_run(struct k_work *work)
{
	static const struct shell_backend_config_flags flags = SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;
#ifdef CONFIG_BRICKPHYR_HOST_UART
	static struct host_protocol protocol;
	static uint8_t reply[HOST_PROTOCOL_FRAME_MAX];
	static size_t reply_length;
	static uint8_t event_reply[HOST_PROTOCOL_FRAME_MAX];
	static size_t event_length;
#endif
	static uint32_t session_generation;
	static bool shell_initialized;

	k_spinlock_key_t key = k_spin_lock(&lock);
	bool changed = session_generation != generation;
	bool open = occupied;
	bool ble = ble_occupied;
	uint8_t selected = stream_mode();
	uint32_t current_generation = generation;
	if (changed) {
		running = false;
	}
	k_spin_unlock(&lock, key);
	if (changed) {
		if (shell_initialized) {
			/* Join accepted commands and clear session state without the
			 * IRQ-masked permission sweep of thread destruction.
			 */
			int err = shell_reset(&host_shell, flags);
			if (err) {
				key = k_spin_lock(&lock);
				if (!first_error) {
					first_error = err;
				}
				k_spin_unlock(&lock, key);
			}
		}
		/* BLE Shell and SMP share the connection's resources. Closing or
		 * starting just the Shell must not clean up an accepted SMP command.
		 * finish_ble_session joins both paths before releasing their owner. */
		if (!ble) {
			application_session_closed();
			hub_matrix_session_closed();
			hub_sound_session_closed();
			hub_ports_session_closed(BP_PORT_TERMINAL);
		}
		smp_shell_rx_reset(&smp_input, &smp_serial);
#ifdef CONFIG_BRICKPHYR_HOST_UART
		host_protocol_reset(&protocol);
		reply_length = 0;
		event_length = 0;
#endif
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
				hub_power_connection(HUB_DISCONNECTED);
			}
			notify_ble_available();
		}
		k_spin_unlock(&lock, key);
		return;
	}
	if (selected == HOST_SHELL && (changed || !shell_initialized)) {
		shell_generation = session_generation;
		managed_shell = false;
		int err = shell_initialized ? shell_start(&host_shell) :
			shell_init(&host_shell, NULL, flags, false, 0);
		if (err) {
			key = k_spin_lock(&lock);
			if (!first_error) {
				first_error = err;
			}
			k_spin_unlock(&lock, key);
			return;
		}
		shell_initialized = true;
	}
	key = k_spin_lock(&lock);
	if (generation != session_generation) {
		k_spin_unlock(&lock, key);
		wake_host();
		return;
	}
	running = true;
	if (selected == HOST_SHELL) {
		notify_shell(SHELL_TRANSPORT_EVT_RX_RDY);
		k_spin_unlock(&lock, key);
		return;
	}
	k_spin_unlock(&lock, key);
#ifdef CONFIG_BRICKPHYR_HOST_UART
	/* Keep one pending event separate from command replies. Unread stdout
	 * must not prevent consuming a STOP/input command. Once its reply is
	 * pending, pause further commands until it can be queued intact.
	 * Bound each batch by the receive capacity so queued management work
	 * can run even when a peer continuously supplies input or drains output. */
	size_t budget = ring_buf_capacity_get(&receive_buffer);
	for (;;) {
		if (!budget--) {
			wake_host();
			break;
		}
		bool queued = false;
		if (event_length && !host_protocol_event_valid(&protocol)) {
			event_length = 0;
			protocol.message_request = 0;
		}
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
			queued = true;
		}
		if (event_length && ring_buf_space_get(&transmit_buffer) >= event_length) {
			ring_buf_put(&transmit_buffer, event_reply, event_length);
			host_protocol_event_sent(&protocol);
			tx_idle = false;
			event_length = 0;
			queued = true;
		}
		uint8_t byte;
		bool received = ring_buf_get(&receive_buffer, &byte, 1);
		k_spin_unlock(&lock, key);
		if (queued) {
			output_ready();
		}
		if (!received) {
			if (event_length) {
				break;
			}
			uint8_t event[HOST_PROTOCOL_PAYLOAD_MAX];
			size_t length = host_protocol_event(&protocol, false, k_uptime_get_32(),
				event, sizeof(event));
			if (length) {
				event_length = pbio_cobs_encode_prefixed(PBIO_PYBRICKS_IN_EP_MSG_EVENT,
					event, length, event_reply);
				continue;
			}
			break;
		}
		reply_length = host_protocol_receive(&protocol, byte, variant, k_uptime_get_32(), reply);
		if (protocol.exit_compatibility) {
			key = k_spin_lock(&lock);
			if (generation == session_generation) {
				next_mode = HOST_SHELL;
			}
			k_spin_unlock(&lock, key);
			protocol.exit_compatibility = false;
		}
	}
	serial_rx_resume();
	if (protocol.subscribed) {
		/* schedule preserves an immediate wake queued during this batch. */
		k_work_schedule_for_queue(smp_get_wq(), &host_work,
			K_MSEC(IS_ENABLED(CONFIG_BRICKPHYR_PYBRICKS_EXECUTION) ?
				20 : HOST_PROTOCOL_STATUS_INTERVAL_MS));
	}
#endif
}

int host_transport_init(const struct device *device, uint16_t hub_variant)
{
	if (device && !IS_ENABLED(CONFIG_BRICKPHYR_HOST_UART)) {
		return -ENOTSUP;
	}
	uart = device;
	variant = hub_variant;
	if (uart && !device_is_ready(uart)) {
		return -ENODEV;
	}
	k_fifo_init(&smp_input.buf_ready);
	smp_transport.functions.output = smp_output;
	smp_transport.functions.get_mtu = smp_mtu;
	if (uart) {
#ifdef CONFIG_BRICKPHYR_HOST_UART
		int err = uart_irq_callback_user_data_set(uart, uart_callback, NULL);
		if (err) return err;
#else
		return -ENOTSUP;
#endif
	}
	mgmt_callback_register(&command_access);
	mgmt_callback_register(&settings_access);
#ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
	mgmt_callback_register(&reset_access);
#endif
	serial_rx_resume();
	/* Establish initial completion in the UART callback without requiring
	 * a USB status request before the first Bluetooth connection. */
	serial_tx_resume();
	wake_host();
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
	wake_host();
	k_sem_give(&smp_tx_ready);
}

void host_connection_changed(bool open)
{
	if (!uart) return;
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
		int err = serial_drain();
		if (err != -1 && !first_error) {
			first_error = err;
		}
	}
	usb_open = open;
	if (!open) hub_power_link_closed(HUB_USB);
	if (!ble_occupied && open != occupied &&
	    (!open || (!first_error && hub_power_connection(HUB_USB)))) {
		if (!open) capture_session_closed();
		stream_changed(open);
	}
	notify_ble_available();
	k_spin_unlock(&lock, key);
	serial_rx_resume();
}

void host_interface_status(uint8_t state[8])
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	state[0] = 1;
	state[1] = mode;
	state[2] = occupied || ble_occupied;
	state[3] = uart && ready && tx_idle && !ble_occupied && !first_error;
	sys_put_le32(first_error, state + 4);
	k_spin_unlock(&lock, key);
	/* Refresh completion in the UART callback, the API's required context. */
	serial_tx_resume();
}

bool host_session_open(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool open = ble_occupied ? !ble_closing : occupied && usb_open;
	k_spin_unlock(&lock, key);
	return open;
}

int host_interface_select(uint16_t requested)
{
	if (!uart || requested > HOST_SHELL) {
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

bool host_ble_available(struct k_sem *changed)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	ble_idle_wake = changed;
	bool available = ble_available();
	k_spin_unlock(&lock, key);
	return available;
}

int host_ble_acquire(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err = 0;
	if (!ble_available()) {
		err = -EBUSY;
	} else if (!hub_power_connection(HUB_BLE)) {
		err = -ESHUTDOWN;
	} else {
		ble_occupied = true;
		ble_managed = false;
	}
	k_spin_unlock(&lock, key);
	return err;
}

static void finish_ble_session(void)
{
	if (ble_closing && !ble_request_thread && !ble_shell) {
		application_session_closed();
		hub_matrix_session_closed();
		hub_sound_session_closed();
		hub_ports_session_closed(BP_PORT_TERMINAL);
		ble_occupied = false;
		ble_closing = false;
		hub_power_connection(HUB_DISCONNECTED);
		if (ble_exit) {
			mode = HOST_SHELL;
		}
		ble_exit = false;
		notify_ble_available();
	}
}

void host_ble_release(bool exit_compatibility)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (ble_occupied && !ble_closing) {
		hub_power_link_closed(HUB_BLE);
		ble_closing = true;
		capture_session_closed();
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
	bool valid = ble_occupied && !ble_closing && !ble_request_thread;
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
	} else if (ble_shell || !ready || first_error) {
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
