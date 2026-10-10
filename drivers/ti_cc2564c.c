// SPDX-License-Identifier: Apache-2.0
/* CC2564C vendor setup over Zephyr's existing H:4 transport and HCI host. */
#include <string.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ti_cc2564c, LOG_LEVEL_INF);

BUILD_ASSERT(CONFIG_BT_BUF_CMD_TX_SIZE >= 255, "TI service pack uses full-size HCI commands");
BUILD_ASSERT(DT_PROP(DT_PARENT(DT_CHOSEN(zephyr_bt_hci)), current_speed) == 115200,
             "The initial TI script and UART must agree on baud rate");

/* The unmodified official TI artifact is included at build time. Its license
 * and provenance are supplied by the pinned service-packs repository. */
static const uint8_t service_pack[] = {
#include <ti_cc2564c_bts.inc>
};

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
		case 6: /* Comment. */
			break;
		default:
			return -ENOTSUP;
		}
		offset += length;
	}
	return pending ? -EINVAL : 0;
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
	if (!err) {
		LOG_INF("CC2564C service pack loaded; eHCILL disabled");
	}
	return err;
}
