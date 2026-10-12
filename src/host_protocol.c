// SPDX-License-Identifier: Apache-2.0
#include "host_protocol.h"
#include "hub.h"

#include <string.h>
#include <pbio/protocol.h>
#include <pbio/util.h>
#include <zephyr/app_version.h>
#ifdef CONFIG_BRICKPHYR_PYTHON
#include "application.h"
#include "python_runtime.h"
#include <errno.h>
#endif

#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
#include "app_store.h"
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static struct k_spinlock slot_lock;
static uint8_t selected_slot;
static bool slot_busy;
/* Flash operations stay off Bluetooth RX and the watchdog's system queue.
 * The existing filesystem management path uses 4 KiB; an ARM check exhausted
 * a 2 KiB worker during LittleFS access. Keep this independent of Shell size. */
static struct k_work_q slot_queue;
static K_THREAD_STACK_DEFINE(slot_stack, 4096);
static K_SEM_DEFINE(slot_done, 0, 1);
static bool slot_install;
static uint8_t slot_target;
static int slot_result;

static void store_slot(struct k_work *work)
{
	char name[5];
	snprintk(name, sizeof(name), "@%u", slot_target);
	slot_result = slot_install ? app_store_publish_upload(name) : app_store_run(name);
	k_sem_give(&slot_done);
}
static K_WORK_DEFINE(slot_work, store_slot);

static int slot_operation(bool install, uint8_t slot)
{
	/* The reservation remains held through this synchronous operation. */
	slot_install = install;
	slot_target = slot;
	int err = k_work_submit_to_queue(&slot_queue, &slot_work);
	if (err < 0) {
		return err;
	}
	k_sem_take(&slot_done, K_FOREVER);
	return slot_result;
}

static int slot_reserve(int requested)
{
	k_spinlock_key_t key = k_spin_lock(&slot_lock);
	int result = slot_busy ? -EBUSY : requested < 0 ? selected_slot : requested;
	if (result >= 0) {
		slot_busy = true;
	}
	k_spin_unlock(&slot_lock, key);
	return result;
}

static void slot_release(void)
{
	k_spinlock_key_t key = k_spin_lock(&slot_lock);
	slot_busy = false;
	k_spin_unlock(&slot_lock, key);
}

uint8_t host_protocol_selected_slot(void)
{
	k_spinlock_key_t key = k_spin_lock(&slot_lock);
	uint8_t result = selected_slot;
	k_spin_unlock(&slot_lock, key);
	return result;
}

int host_protocol_select_slot(unsigned int slot)
{
	if (slot >= CONFIG_BRICKPHYR_PYBRICKS_SLOTS) {
		return -EINVAL;
	}
	struct application_status status;
	if (application_observe(&status) || status.state == APPLICATION_RUNNING) {
		return -EBUSY;
	}
	k_spinlock_key_t key = k_spin_lock(&slot_lock);
	int err = slot_busy ? -EBUSY : 0;
	if (!err) {
		selected_slot = slot;
	}
	k_spin_unlock(&slot_lock, key);
	return err;
}

static int cmd_slot(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 2) {
		int err = 0;
		unsigned long slot = shell_strtoul(argv[1], 10, &err);
		if (!err) {
			err = slot > 127 ? -EINVAL : host_protocol_select_slot(slot);
		}
		if (err) {
			shell_error(shell, "Slot selection: %d", err);
			return err;
		}
	}
	shell_print(shell, "Pybricks slot: %u of %u (zero-based)",
		host_protocol_selected_slot(), CONFIG_BRICKPHYR_PYBRICKS_SLOTS);
	return 0;
}
SHELL_CMD_ARG_REGISTER(pybricks_slot, NULL, "Read or select a Pybricks program slot.", cmd_slot, 1, 1);

static int slots_init(void)
{
	const struct k_work_queue_config config = {.name = "pybricks_store"};
	k_work_queue_start(&slot_queue, slot_stack, K_THREAD_STACK_SIZEOF(slot_stack),
		CONFIG_SHELL_THREAD_PRIORITY, &config);
	return 0;
}
SYS_INIT(slots_init, APPLICATION, 90);
#endif

void host_protocol_reset(struct host_protocol *protocol)
{
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
	if (protocol->downloading) {
		slot_release();
	}
#endif
	*protocol = (struct host_protocol){0};
}

size_t host_protocol_read(uint8_t service, uint16_t id, uint16_t variant,
			  uint16_t max_write, uint8_t value[HOST_PROTOCOL_PAYLOAD_MAX - 3])
{
	const char *text = NULL;

	if (service == PBIO_PYBRICKS_READ_SERVICE_GATT) {
		switch (id) {
		case PBIO_GATT_DEVICE_NAME_CHAR_UUID:
			text = hub_name();
			break;
		case PBIO_GATT_FIRMWARE_VERSION_CHAR_UUID:
			text = APP_VERSION_STRING;
			break;
		case PBIO_GATT_SOFTWARE_VERSION_CHAR_UUID:
			text = PBIO_PROTOCOL_VERSION_STR;
			break;
		case PBIO_GATT_PNP_ID_CHAR_UUID:
			pbio_pybricks_pnp_id(value, PBIO_PYBRICKS_HUB_KIND_PRIME, variant);
			return PBIO_PYBRICKS_PNP_ID_SIZE;
		}
	} else if (service == PBIO_PYBRICKS_READ_SERVICE_PYBRICKS && id == 0x0003) {
#ifdef CONFIG_BRICKPHYR_PYTHON
		/* Leave room for file-map headers when normalizing up to 64 modules. */
		pbio_pybricks_hub_capabilities(value, max_write,
			PBIO_PYBRICKS_FEATURE_FLAG_USER_PROG_FORMAT_MULTI_MPY_V6 |
			(IS_ENABLED(CONFIG_CPU_CORTEX_M) ?
			 PBIO_PYBRICKS_FEATURE_FLAG_USER_PROG_FORMAT_MULTI_MPY_V6_3_NATIVE : 0) |
			PBIO_PYBRICKS_FEATURE_FLAG_BUILTIN_USER_PROGRAM_REPL,
			PYTHON_PYBRICKS_LIMIT,
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
			CONFIG_BRICKPHYR_PYBRICKS_SLOTS);
#else
			0);
#endif
#else
		pbio_pybricks_hub_capabilities(value, max_write, 0, 0, 0);
#endif
		return PBIO_PYBRICKS_HUB_CAPABILITIES_VALUE_SIZE;
	}
	if (!text) {
		return 0;
	}
	size_t length = strlen(text);
	if (length > HOST_PROTOCOL_PAYLOAD_MAX - 3) {
		length = HOST_PROTOCOL_PAYLOAD_MAX - 3;
	}
	memcpy(value, text, length);
	if (service == PBIO_PYBRICKS_READ_SERVICE_GATT &&
	    id == PBIO_GATT_FIRMWARE_VERSION_CHAR_UUID) {
		/* PEP 440 clients accept our bring-up label as local version metadata. */
		uint8_t *separator = memchr(value, '-', length);
		if (separator) {
			*separator = '+';
		}
	}
	return length;
}

uint8_t host_protocol_command(struct host_protocol *protocol, const uint8_t *command, size_t length)
{
	if (!length) {
		return PBIO_PYBRICKS_ERROR_INVALID_COMMAND;
	}
	if (command[0] == HOST_COMMAND_EXIT_COMPATIBILITY) {
		if (length != 1) {
			return PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED;
		}
		protocol->exit_compatibility = true;
		return PBIO_PYBRICKS_ERROR_OK;
	}
#ifdef CONFIG_BRICKPHYR_PYTHON
	struct application_status status;
	application_get_status(&status);
	int err;
	switch (command[0]) {
	case PBIO_PYBRICKS_COMMAND_STOP_USER_PROGRAM:
		if (length != 1) {
			return PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED;
		}
		err = application_stop();
		break;
	case PBIO_PYBRICKS_COMMAND_START_REPL:
	case PBIO_PYBRICKS_COMMAND_START_USER_PROGRAM: {
		bool repl = command[0] == PBIO_PYBRICKS_COMMAND_START_REPL;
		if (length > (repl ? 1 : 2)) {
			return PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED;
		}
		uint8_t id = repl ? PBIO_PYBRICKS_USER_PROGRAM_ID_REPL : length == 2 ? command[1] : 0;
		if (id != PBIO_PYBRICKS_USER_PROGRAM_ID_REPL &&
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
		    id >= CONFIG_BRICKPHYR_PYBRICKS_SLOTS) {
#else
		    id != 0) {
#endif
			return PBIO_PYBRICKS_ERROR_INVALID_COMMAND;
		}
		if (status.state == APPLICATION_RUNNING || protocol->downloading) {
			return PBIO_PYBRICKS_ERROR_BUSY;
		}
		if (id == PBIO_PYBRICKS_USER_PROGRAM_ID_REPL) {
			err = application_repl();
		} else {
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
			int slot = slot_reserve(length == 2 ? id : -1);
			if (slot < 0) {
				return PBIO_PYBRICKS_ERROR_BUSY;
			}
			id = slot;
			err = slot_operation(false, id);
			slot_release();
#else
			if (status.upload_ready && status.upload_format == APPLICATION_NATIVE) {
				return PBIO_PYBRICKS_ERROR_INVALID_COMMAND;
			}
			err = application_run_uploaded(true);
#endif
		}
		if (err == -ENOENT || err == -ENODATA) {
			return PBIO_PYBRICKS_ERROR_INVALID_COMMAND;
		}
		if (!err) {
			protocol->start_pending = true;
			protocol->program_id = id;
		}
		break;
	}
	case PBIO_PYBRICKS_COMMAND_WRITE_USER_PROGRAM_META: {
		if (length != 5) {
			return PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED;
		}
		if (status.state == APPLICATION_RUNNING) {
			return PBIO_PYBRICKS_ERROR_BUSY;
		}
		uint32_t total = pbio_get_uint32_le(command + 1);
		if (!total) {
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
			if (!protocol->downloading) {
				int slot = slot_reserve(-1);
				if (slot < 0) {
					return PBIO_PYBRICKS_ERROR_BUSY;
				}
				protocol->download_slot = slot;
			}
#endif
			application_discard_upload();
			protocol->downloading = true;
			protocol->download_bytes = 0;
			err = 0;
		} else if (!protocol->downloading || total != protocol->download_bytes) {
			err = -EINVAL;
		} else {
			err = application_upload_finish(total);
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
			if (!err) {
				err = slot_operation(true, protocol->download_slot);
			}
			slot_release();
#endif
			protocol->downloading = false;
		}
		break;
	}
	case PBIO_PYBRICKS_COMMAND_WRITE_USER_RAM:
		if (length < 5 || !protocol->downloading) {
			return PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED;
		}
		if (status.state == APPLICATION_RUNNING) {
			return PBIO_PYBRICKS_ERROR_BUSY;
		}
		if (length == 5) {
			return pbio_get_uint32_le(command + 1) <= protocol->download_bytes ?
				PBIO_PYBRICKS_ERROR_OK : PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED;
		}
		err = application_upload(APPLICATION_PYBRICKS, pbio_get_uint32_le(command + 1),
			0, command + 5, length - 5, &protocol->download_bytes);
		break;
	case PBIO_PYBRICKS_COMMAND_WRITE_STDIN:
		err = length == 1 ? 0 : application_input(command + 1, length - 1);
		break;
	default:
		return PBIO_PYBRICKS_ERROR_INVALID_COMMAND;
	}
	if (!err) {
		return PBIO_PYBRICKS_ERROR_OK;
	}
	if (err == -EBUSY || err == -EAGAIN || err == -ENOMEM) {
		return PBIO_PYBRICKS_ERROR_BUSY;
	}
	return err == -EIO || err == -ETIMEDOUT ? PBIO_PYBRICKS_ERROR_UNLIKELY_ERROR :
		PBIO_PYBRICKS_ERROR_VALUE_NOT_ALLOWED;
#else
	return PBIO_PYBRICKS_ERROR_INVALID_COMMAND;
#endif
}

size_t host_protocol_event(struct host_protocol *protocol, bool ble,
	uint32_t now_ms, uint8_t *output, size_t capacity)
{
	if (!protocol->subscribed || capacity < PBIO_PYBRICKS_EVENT_STATUS_REPORT_SIZE) {
		return 0;
	}
	uint32_t flags = 1U << (ble ? PBIO_PYBRICKS_STATUS_BLE_HOST_CONNECTED :
		PBIO_PYBRICKS_STATUS_USB_HOST_CONNECTED);
	bool status_due = now_ms - protocol->last_status_ms >= HOST_PROTOCOL_STATUS_INTERVAL_MS;
	uint8_t slot = 0;
#if CONFIG_BRICKPHYR_PYBRICKS_SLOTS > 0
	k_spinlock_key_t key = k_spin_lock(&slot_lock);
	slot = selected_slot;
	if (slot_busy) {
		flags |= 1U << PBIO_PYBRICKS_STATUS_FILE_IO_IN_PROGRESS;
	}
	k_spin_unlock(&slot_lock, key);
#endif
#ifdef CONFIG_BRICKPHYR_PYTHON
	struct application_status status;
	if (application_observe(&status)) {
		return 0;
	}
	if (protocol->start_pending || status.state == APPLICATION_RUNNING) {
		flags |= 1U << PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING;
	}
	/* A periodic running report must not wait behind continuous stdout.
	 * Drain a completed program's final output before reporting it stopped:
	 * clients may stop reading as soon as they see that transition. */
	if (!protocol->start_pending && !protocol->status_dirty &&
	    (!status_due || status.state != APPLICATION_RUNNING)) {
		size_t length = application_output(output + 1, capacity - 1);
		if (length) {
			output[0] = PBIO_PYBRICKS_EVENT_WRITE_STDOUT;
			return length + 1;
		}
	}
#endif
	if (status_due || protocol->status_dirty || protocol->start_pending || flags != protocol->last_status ||
	    slot != protocol->last_slot) {
		protocol->status_dirty = protocol->start_pending = false;
		protocol->last_status = flags;
		protocol->last_status_ms = now_ms;
		protocol->last_slot = slot;
		return pbio_pybricks_event_status_report(output, flags, protocol->program_id, slot);
	}
	return 0;
}

size_t host_protocol_receive(struct host_protocol *protocol, uint8_t byte,
			    uint16_t hub_variant, uint32_t now_ms,
			    uint8_t output[HOST_PROTOCOL_FRAME_MAX])
{
	if (byte != PBIO_COBS_DELIMITER) {
		if (protocol->length < sizeof(protocol->frame)) {
			protocol->frame[protocol->length++] = byte;
		} else {
			protocol->overflow = true;
		}
		return 0;
	}
	uint8_t type;
	uint8_t message[HOST_PROTOCOL_PAYLOAD_MAX];
	size_t length = protocol->overflow ? 0 : pbio_cobs_decode_prefixed(
		protocol->frame, protocol->length, &type, message, sizeof(message));
	bool empty = protocol->length == 0;
	protocol->length = 0;
	protocol->overflow = false;
	if (!length) {
		protocol->rejected_frames += !empty;
		return 0;
	}

	uint8_t reply[HOST_PROTOCOL_PAYLOAD_MAX];
	if (type == PBIO_PYBRICKS_OUT_EP_MSG_READ && length == 3) {
		memcpy(reply, message, 3);
		length = 3 + host_protocol_read(message[0], pbio_get_uint16_le(message + 1),
			hub_variant, HOST_PROTOCOL_PAYLOAD_MAX - 1, reply + 3);
		return pbio_cobs_encode_prefixed(PBIO_PYBRICKS_IN_EP_MSG_READ_REPLY,
						reply, length, output);
	}
	if (type == PBIO_PYBRICKS_OUT_EP_MSG_COMMAND && length >= 2) {
		reply[0] = message[0];
		uint8_t error = host_protocol_command(protocol, message + 1, length - 1);
		pbio_set_uint32_le(reply + 1, error);
		return pbio_cobs_encode_prefixed(PBIO_PYBRICKS_IN_EP_MSG_RESPONSE,
						reply, 5, output);
	}
	if (type == PBIO_PYBRICKS_OUT_EP_MSG_SUBSCRIBE && length == 1 && message[0] <= 1) {
		protocol->subscribed = message[0];
#ifdef CONFIG_BRICKPHYR_PYTHON
		application_output_stream(protocol->subscribed);
#endif
		if (!message[0]) {
			return 0;
		}
		protocol->status_dirty = true;
		length = host_protocol_event(protocol, false, now_ms, reply, sizeof(reply));
		if (!length) {
			return 0;
		}
		return pbio_cobs_encode_prefixed(PBIO_PYBRICKS_IN_EP_MSG_EVENT,
						reply, length, output);
	}
	/* The upstream framing has no error reply for malformed reads or envelopes. */
	protocol->rejected_frames++;
	return 0;
}
