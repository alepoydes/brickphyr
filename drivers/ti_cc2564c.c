// SPDX-License-Identifier: Apache-2.0
/* CC2564C vendor setup over Zephyr's existing H:4 transport and HCI host. */
#include "hub.h"

#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ti_cc2564c, LOG_LEVEL_INF);

BUILD_ASSERT(CONFIG_BT_BUF_CMD_TX_SIZE >= 255, "TI service pack uses full-size HCI commands");

#define HCI_UART_BAUDRATE 921600U

/* The unmodified official TI artifact is included at build time. Its license
 * and provenance are supplied by the pinned service-packs repository. */
static const uint8_t service_pack[] = {
#include <ti_cc2564c_bts.inc>
};
static const char *firmware_version;
static size_t firmware_version_length;
static bool version_scanned;

int hub_bluetooth_version(char *value, size_t size)
{
	if (!firmware_version) return -ENODATA;
	if (size <= firmware_version_length) return -ENOSPC;
	if (!value) return -EINVAL;
	memcpy(value, firmware_version, firmware_version_length);
	value[firmware_version_length] = '\0';
	return 0;
}

static int send_command(uint16_t opcode, const uint8_t *data, size_t length)
{
	struct net_buf *command = bt_hci_cmd_alloc(K_NO_WAIT);
	if (!command) {
		return -ENOBUFS;
	}
	net_buf_add_mem(command, data, length);
	if (opcode == 0xfd0c) {
		/* TI's documented Deep Sleep Enable parameter. Standard H:4 does
		 * not implement the eHCILL handshake; leave controller deep sleep off.
		 * RAM patch commands and the embedded BTS artifact remain unchanged. */
		command->data[1] = 0;
	}
	int err = bt_hci_cmd_send_sync(opcode, command, NULL);
	if (err) {
		LOG_ERR("Service pack command 0x%04x failed: %d", opcode, err);
	}
	return err;
}

static int load_service_pack(bool apply)
{
	if (sizeof(service_pack) < 32 || memcmp(service_pack, "BTSB", 4) ||
	    sys_get_le32(service_pack + 4) != 1) {
		return -EINVAL;
	}
	uint16_t pending = 0;
	size_t offset = 32;
	const char *version = NULL;
	size_t version_length = 0;
	while (offset < sizeof(service_pack)) {
		if (sizeof(service_pack) - offset < 4) {
			return -EINVAL;
		}
		uint16_t type = sys_get_le16(service_pack + offset);
		size_t length = sys_get_le16(service_pack + offset + 2);
		offset += 4;
		if (length > sizeof(service_pack) - offset) {
			return -EINVAL;
		}
		const uint8_t *data = service_pack + offset;
		switch (type) {
		case 1: /* H:4 command, followed by its expected completion. */
			if (pending || length < 4 || data[0] != BT_HCI_H4_CMD ||
			    length != 4 + data[3]) {
				return -EINVAL;
			}
			pending = sys_get_le16(data + 1);
			if (!pending || (pending == 0xfd0c && data[3] != 9)) {
				return -EINVAL;
			}
			if (pending == 0xff36 &&
			    (data[3] != 4 || sys_get_le32(data + 4) != 115200)) {
				return -ENOTSUP;
			}
			if (apply) {
				int err = send_command(pending, data + 4, data[3]);
				if (err) {
					return err;
				}
			}
			break;
		case 2:
			/* This pinned script expects successful Command Complete events.
			 * Zephyr correlates them and owns the synchronous HCI timeout. */
			if (!pending || length != 15 || sys_get_le32(data) != 5000 ||
			    sys_get_le32(data + 4) != 7 || data[8] != BT_HCI_H4_EVT ||
			    data[9] != BT_HCI_EVT_CMD_COMPLETE || data[10] != 4 ||
			    sys_get_le16(data + 12) != pending || data[14]) {
				return -EINVAL;
			}
			pending = 0;
			break;
		case 3: /* Host serial parameters already selected in devicetree. */
			if (pending || length != 8 || sys_get_le32(data) != 115200 ||
			    sys_get_le32(data + 4) != 1) {
				return -ENOTSUP;
			}
			break;
		case 6: { /* Comment. */
			static const char prefix[] = "# Version     : ";
			if (!apply && !version_scanned && !version && length > sizeof(prefix) - 1 &&
			    !memcmp(data, prefix, sizeof(prefix) - 1)) {
				const char *start = (const char *)data + sizeof(prefix) - 1;
				size_t available = length - (sizeof(prefix) - 1);
				const char *end = memchr(start, '\0', available);
				version_length = end ? (size_t)(end - start) : available;
				if (version_length) version = start;
			}
			break;
		}
		default:
			return -ENOTSUP;
		}
		offset += length;
	}
	if (pending) return -EINVAL;
	if (!apply && !version_scanned) {
		firmware_version = version;
		firmware_version_length = version_length;
		version_scanned = true;
	}
	return 0;
}

int bt_h4_vnd_setup(const struct device *uart, const struct bt_hci_setup_params *params)
{
	/* Validate the complete artifact before sending any of its commands. */
	int err = load_service_pack(false);
	if (err) {
		LOG_ERR("Unsupported or invalid TI service pack: %d", err);
		return err;
	}
	err = bt_hci_cmd_send_sync(BT_HCI_OP_RESET, NULL, NULL);
	if (err) {
		return err;
	}
	struct net_buf *reply;
	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_LOCAL_VERSION_INFO, NULL, &reply);
	if (err) {
		return err;
	}
	if (reply->len != sizeof(struct bt_hci_rp_read_local_version_info)) {
		net_buf_unref(reply);
		return -EBADMSG;
	}
	const struct bt_hci_rp_read_local_version_info *version = (const void *)reply->data;
	uint16_t manufacturer = sys_le16_to_cpu(version->manufacturer);
	uint16_t rom = sys_le16_to_cpu(version->lmp_subversion);
	net_buf_unref(reply);
	if (manufacturer != 0x000d || rom != 0x9a1a) {
		LOG_ERR("Service pack requires TI CC2564C: manufacturer 0x%04x, ROM 0x%04x",
			manufacturer, rom);
		return -ENODEV;
	}
	err = load_service_pack(true);
	if (err) {
		return err;
	}

	/* The TI script starts at 115200. Change both ends only after its final
	 * command completes at that speed, as in the Pybricks CC2564C setup. */
	struct uart_config config;
	err = uart_config_get(uart, &config);
	if (err) {
		return err;
	}
	uint8_t baud[4];
	sys_put_le32(HCI_UART_BAUDRATE, baud);
	err = send_command(0xff36, baud, sizeof(baud));
	if (err) {
		return err;
	}
	config.baudrate = HCI_UART_BAUDRATE;
	err = uart_configure(uart, &config);
	if (err) {
		LOG_ERR("HCI UART baud change failed: %d", err);
		return err;
	}
	/* Fail startup immediately if the controller and STM32 disagree. */
	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_LOCAL_VERSION_INFO, NULL, &reply);
	if (err) {
		LOG_ERR("HCI UART verification failed at %u baud: %d", HCI_UART_BAUDRATE, err);
		return err;
	}
	net_buf_unref(reply);
	LOG_INF("CC2564C service pack loaded; UART %u baud; eHCILL disabled",
		HCI_UART_BAUDRATE);
	return 0;
}
