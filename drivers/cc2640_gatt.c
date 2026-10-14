// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
/* Service registration and ATT handling adapted from pybricks-micropython
 * a7b1b04ae4f8449dd46b3dfb97c420334439fd35,
 * lib/pbio/drv/bluetooth/bluetooth_stm32_cc2640.c.
 * A table replaces the handle-specific branches; range/length/permission checks
 * precede access to the shared Brickphyr BLE session and Shell. Registration and
 * replies use the TI network-processor wire format without its restricted ATT
 * headers or unmarked encoders. See LICENSES/Pybricks-MIT.txt. */

#include "cc2640_gatt.h"
#include "host_protocol.h"
#include "host_cc2640_smp.h"

#include <errno.h>
#include <string.h>
#include <pbio/protocol.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/sys/byteorder.h>

enum value_kind { DECLARATION, NAME, APPEARANCE, PARAMETERS,
	FW_VERSION, PROTOCOL_VERSION, PNP, PYBRICKS_COMMAND, CAPABILITIES,
	PYBRICKS_CCC, NUS_TX, NUS_CCC, NUS_RX, SHELL_TX, SHELL_CCC, SHELL_RX,
	SMP_PACKET, SMP_CCC };
enum { READ = 0x02, WRITE_COMMAND = 0x04, WRITE = 0x08, NOTIFY = 0x10 };
/* Bluetooth Core ATT opcodes. Public Zephyr att.h supplies error codes but does
 * not export these wire constants; no private host-stack header is needed. */
enum att_opcode {
	ATT_ERROR_RSP = 0x01, ATT_MTU_REQ = 0x02, ATT_MTU_RSP = 0x03,
	ATT_FIND_INFO_REQ = 0x04, ATT_FIND_INFO_RSP = 0x05,
	ATT_FIND_TYPE_REQ = 0x06, ATT_FIND_TYPE_RSP = 0x07,
	ATT_READ_TYPE_REQ = 0x08, ATT_READ_REQ = 0x0a, ATT_READ_BLOB_REQ = 0x0c,
	ATT_READ_GROUP_REQ = 0x10, ATT_WRITE_REQ = 0x12, ATT_WRITE_RSP = 0x13,
	ATT_PREPARE_WRITE_REQ = 0x16, ATT_EXEC_WRITE_REQ = 0x18, ATT_EXEC_WRITE_RSP = 0x19,
};

/* UUIDs use Bluetooth wire order. Pybricks' canonical UUIDs are reversed by
 * uuid_copy; the other services use Zephyr's standard UUID encodings. */
static const uint8_t nus_uuid[][16] = {
	{BT_UUID_NUS_SRV_VAL}, {BT_UUID_NUS_TX_CHAR_VAL}, {BT_UUID_NUS_RX_CHAR_VAL},
};
static const uint8_t shell_uuid[][16] = {
	{HOST_BLE_SHELL_UUID(0)}, {HOST_BLE_SHELL_UUID(1)}, {HOST_BLE_SHELL_UUID(2)},
};
struct attribute {
	uint16_t uuid16;
	uint8_t kind, properties;
	const uint8_t *uuid128;
};
#define ATTR(uuid, kind_, props) {uuid, kind_, props, NULL}
#define ATTR128(uuid, kind_, props) {0, kind_, props, uuid}
#define DECL ATTR(0x2803, DECLARATION, READ)
#define CCC(kind_) ATTR(0x2902, kind_, READ | WRITE)
static const struct attribute gap[] = {
	DECL, ATTR(0x2a00, NAME, READ), DECL, ATTR(0x2a01, APPEARANCE, READ),
	DECL, ATTR(0x2a04, PARAMETERS, READ),
};
static const struct attribute device_info[] = {
	DECL, ATTR(0x2a26, FW_VERSION, READ), DECL, ATTR(0x2a28, PROTOCOL_VERSION, READ),
	DECL, ATTR(0x2a50, PNP, READ),
};
static const struct attribute pybricks[] = {
	DECL, ATTR128(pbio_pybricks_command_event_char_uuid, PYBRICKS_COMMAND, WRITE | NOTIFY),
	CCC(PYBRICKS_CCC), DECL, ATTR128(pbio_pybricks_hub_capabilities_char_uuid, CAPABILITIES, READ),
};
/* Current Pybricks clients subscribe to NUS, while program I/O uses the command
 * characteristic. Preserve F4 discovery/CCC behavior; legacy stdin is unsupported. */
static const struct attribute nus[] = {
	DECL, ATTR128(nus_uuid[1], NUS_TX, NOTIFY), CCC(NUS_CCC),
	DECL, ATTR128(nus_uuid[2], NUS_RX, WRITE),
};
static const struct attribute shell[] = {
	DECL, ATTR128(shell_uuid[1], SHELL_TX, NOTIFY), CCC(SHELL_CCC),
	DECL, ATTR128(shell_uuid[2], SHELL_RX, WRITE),
};
#ifdef CONFIG_BRICKPHYR_CC2640_SMP
static const uint8_t smp_uuid[][16] = {{SMP_BT_SVC_UUID_VAL}, {SMP_BT_CHR_UUID_VAL}};
static const struct attribute smp[] = {
	DECL, ATTR128(smp_uuid[1], SMP_PACKET, WRITE_COMMAND | NOTIFY), CCC(SMP_CCC),
};
#endif
static const struct {
	uint16_t uuid16;
	uint8_t count;
	const uint8_t *uuid128;
	const struct attribute *attributes;
} services[] = {
	{0x1801, 1, NULL, NULL},
	{0x1800, 1 + ARRAY_SIZE(gap), NULL, gap},
	{0x180a, 1 + ARRAY_SIZE(device_info), NULL, device_info},
	{0, 1 + ARRAY_SIZE(pybricks), pbio_pybricks_service_uuid, pybricks},
	{0, 1 + ARRAY_SIZE(nus), nus_uuid[0], nus},
	{0, 1 + ARRAY_SIZE(shell), shell_uuid[0], shell},
#ifdef CONFIG_BRICKPHYR_CC2640_SMP
	{0, 1 + ARRAY_SIZE(smp), smp_uuid[0], smp},
#endif
};
BUILD_ASSERT(ARRAY_SIZE(services) == CC2640_GATT_SERVICE_COUNT);

static size_t uuid_copy(uint8_t *out, uint16_t uuid16, const uint8_t *uuid128)
{
	if (uuid16) {
		sys_put_le16(uuid16, out);
		return 2;
	}
	if (uuid128 == pbio_pybricks_service_uuid ||
	    uuid128 == pbio_pybricks_command_event_char_uuid ||
	    uuid128 == pbio_pybricks_hub_capabilities_char_uuid) {
		for (size_t i = 0; i < 16; i++) out[i] = uuid128[15 - i];
	} else {
		memcpy(out, uuid128, 16);
	}
	return 16;
}

size_t cc2640_gatt_registration(unsigned int service, unsigned int attribute,
			       uint8_t packet[CC2640_GATT_PACKET_MAX])
{
	if (service >= ARRAY_SIZE(services) || attribute >= services[service].count) return 0;
	packet[0] = 1;
	packet[2] = 0xfd;
	if (!attribute) {
		packet[1] = 0xfc; /* GATT_AddService(primary type, attribute count, key size). */
		packet[3] = 5;
		sys_put_le16(0x2800, packet + 4);
		sys_put_le16(services[service].count, packet + 6);
		packet[8] = 7;
	} else {
		packet[1] = 0xfe; /* GATT_AddAttribute(UUID, permissions). */
		const struct attribute *attr = &services[service].attributes[attribute - 1];
		size_t length = uuid_copy(packet + 4, attr->uuid16, attr->uuid128);
		/* Notify-only values still need a registered attribute. Actual read
		 * access is checked against the characteristic's properties below. */
		packet[4 + length] = (attr->properties & (WRITE | WRITE_COMMAND) ? 2 : 0) |
			(attr->properties & (READ | NOTIFY) ? 1 : 0);
		packet[3] = length + 1;
	}
	return packet[3] + 4;
}

int cc2640_gatt_bind(struct cc2640_gatt *gatt, unsigned int service,
		    uint16_t first, uint16_t last)
{
	if (service >= ARRAY_SIZE(services) || !first || last < first ||
	    last - first + 1 != services[service].count) return -EBADMSG;
	for (size_t i = 0; i < ARRAY_SIZE(services); i++) {
		uint16_t start = gatt->first[i];
		if (start && ((i < service && first <= start + services[i].count - 1) ||
		    (i > service && last >= start))) return -EBADMSG;
	}
	gatt->first[service] = first;
	return 0;
}

static int locate(const struct cc2640_gatt *gatt, uint16_t handle, unsigned int *offset)
{
	for (size_t i = 0; i < ARRAY_SIZE(services); i++) {
		if (gatt->first[i] && handle >= gatt->first[i] &&
		    handle - gatt->first[i] < services[i].count) {
			*offset = handle - gatt->first[i];
			return i;
		}
	}
	return -1;
}

static int ccc_index(enum value_kind kind)
{
	return kind == PYBRICKS_CCC ? 0 : kind == NUS_CCC ? 1 :
		kind == SHELL_CCC ? 2 : kind == SMP_CCC ? 3 : -1;
}

static int read_value(struct cc2640_gatt *gatt, struct host_ble_link *link,
		      unsigned int service, unsigned int offset, uint16_t skip, uint8_t *value)
{
	int count;
	if (!offset) {
		count = uuid_copy(value, services[service].uuid16, services[service].uuid128);
		goto slice;
	}
	const struct attribute *attr = &services[service].attributes[offset - 1];
	if (!(attr->properties & READ)) return -BT_ATT_ERR_READ_NOT_PERMITTED;
	if (attr->kind == DECLARATION) {
		const struct attribute *characteristic = attr + 1;
		value[0] = characteristic->properties;
		sys_put_le16(gatt->first[service] + offset + 1, value + 1);
		count = 3 + uuid_copy(value + 3, characteristic->uuid16, characteristic->uuid128);
		goto slice;
	}
	int ccc = ccc_index(attr->kind);
	if (ccc >= 0) {
		sys_put_le16(gatt->subscribed[ccc], value);
		count = 2;
		goto slice;
	}
	if (attr->kind == APPEARANCE) {
		sys_put_le16(0, value);
		count = 2;
		goto slice;
	}
	if (attr->kind == PARAMETERS) {
		/* Reference peripheral preference: 30–60 ms, no skipped events, 5 s
		 * supervision timeout. These are preferences, not measured intervals. */
		sys_put_le16(24, value);
		sys_put_le16(48, value + 2);
		sys_put_le16(0, value + 4);
		sys_put_le16(500, value + 6);
		count = 8;
		goto slice;
	}
	return host_ble_session_read(link, attr->kind == CAPABILITIES,
		attr->kind == CAPABILITIES ? 3 : attr->uuid16,
		value, HOST_PROTOCOL_PAYLOAD_MAX - 3, skip);
slice:
	if (skip > count) return -BT_ATT_ERR_INVALID_OFFSET;
	memmove(value, value + skip, count - skip);
	return count - skip;
}

static int write_value(struct cc2640_gatt *gatt, struct host_ble_link *link,
		       uint16_t handle, const uint8_t *value, size_t length)
{
	unsigned int offset;
	int service = locate(gatt, handle, &offset);
	if (service < 0) return BT_ATT_ERR_INVALID_HANDLE;
	if (!offset) return BT_ATT_ERR_WRITE_NOT_PERMITTED;
	const struct attribute *attr = &services[service].attributes[offset - 1];
	if (!(attr->properties & (WRITE | WRITE_COMMAND))) return BT_ATT_ERR_WRITE_NOT_PERMITTED;
	int ccc = ccc_index(attr->kind);
	int result;
	if (ccc >= 0) {
		if (length != 2) return BT_ATT_ERR_INVALID_ATTRIBUTE_LEN;
		uint16_t enabled = sys_get_le16(value);
		if (enabled > 1) return BT_ATT_ERR_VALUE_NOT_ALLOWED;
#ifdef CONFIG_BRICKPHYR_CC2640_SMP
		if (ccc == 3) {
			host_cc2640_smp_subscribe(link, enabled);
			gatt->subscribed[ccc] = enabled;
			return 0;
		}
#endif
		result = ccc == 2 ? host_ble_session_shell_subscribe(link, enabled) :
			host_ble_session_ccc_check(link, enabled);
		if (result < 0) return -result;
		gatt->subscribed[ccc] = enabled;
		if (ccc == 0) host_ble_session_subscribed(enabled);
		return 0;
	}
	if (attr->kind == PYBRICKS_COMMAND) {
		result = host_ble_session_command(link, value, length, 0, false);
	} else if (attr->kind == SHELL_RX) {
		result = host_ble_session_shell_input(link, value, length, 0, false);
#ifdef CONFIG_BRICKPHYR_CC2640_SMP
	} else if (attr->kind == SMP_PACKET) {
		result = host_cc2640_smp_receive(link, value, length);
#endif
	} else {
		return BT_ATT_ERR_NOT_SUPPORTED; /* Legacy NUS stdin. */
	}
	return result < 0 ? -result : 0;
}

static int reply(uint8_t *packet, uint16_t connection, uint8_t opcode, size_t length)
{
	packet[0] = 1;
	packet[1] = opcode;
	packet[2] = 0xfd;
	packet[3] = length + 2;
	sys_put_le16(connection, packet + 4);
	return length + 6;
}

static int error_reply(uint8_t *packet, uint16_t connection, uint8_t request,
		       uint16_t handle, uint8_t error)
{
	packet[6] = request;
	sys_put_le16(handle, packet + 7);
	packet[9] = error;
	return reply(packet, connection, ATT_ERROR_RSP, 4);
}

int cc2640_gatt_request(struct cc2640_gatt *gatt, struct host_ble_link *link,
		       uint16_t connection, const uint8_t *event, size_t size,
		       uint8_t packet[CC2640_GATT_PACKET_MAX])
{
	/* TI vendor ATT event: HCI header, event LE16, status, connection LE16,
	 * PDU length, then PDU fields without the opcode. Write events additionally
	 * prefix their handle/value with signature and command flags. */
	if (size < 9 || event[0] != 4 || event[1] != 0xff || event[2] != size - 3 ||
	    event[4] != 5 || event[5] || event[8] != size - 9) return -EBADMSG;
	if (sys_get_le16(event + 6) != connection) return -ENOTCONN;
	const uint8_t *data = event + 9;
	size_t length = size - 9;
	uint8_t opcode = event[3];
	if ((opcode & 1) || opcode == 0x1e) return -ENOTSUP;
	uint16_t handle = length >= 2 ? sys_get_le16(data) : 0;
	uint8_t error = BT_ATT_ERR_NOT_SUPPORTED;
	if (opcode == ATT_WRITE_REQ) {
		if (length < 4 || length > CC2640_ATT_MTU + 1 || data[0] > 1 || data[1] > 1) {
			return -EBADMSG;
		}
		handle = sys_get_le16(data + 2);
		error = data[0] ? BT_ATT_ERR_NOT_SUPPORTED :
			write_value(gatt, link, handle, data + 4, length - 4);
		if (data[1]) return 0;
		if (!error) return reply(packet, connection, ATT_WRITE_RSP, 0);
	} else if (opcode == ATT_MTU_REQ) {
		bool valid = length == 2 && handle >= CC2640_ATT_MTU;
		handle = 0;
		if (!valid) error = BT_ATT_ERR_INVALID_PDU;
		else {
			sys_put_le16(CC2640_ATT_MTU, packet + 6);
			return reply(packet, connection, ATT_MTU_RSP, 2);
		}
	} else if (opcode == ATT_READ_REQ || opcode == ATT_READ_BLOB_REQ) {
		if (length != (opcode == ATT_READ_REQ ? 2 : 4)) return -EBADMSG;
		unsigned int offset;
		int service = locate(gatt, handle, &offset);
		uint8_t value[HOST_PROTOCOL_PAYLOAD_MAX - 3];
		uint16_t skip = opcode == ATT_READ_BLOB_REQ ? sys_get_le16(data + 2) : 0;
		int count = service < 0 ? -BT_ATT_ERR_INVALID_HANDLE :
			read_value(gatt, link, service, offset, skip, value);
		if (count < 0) error = -count;
		else {
			count = MIN(count, CC2640_ATT_MTU - 1);
			memcpy(packet + 6, value, count);
			return reply(packet, connection, opcode + 1, count);
		}
	} else if (opcode == ATT_READ_TYPE_REQ || opcode == ATT_READ_GROUP_REQ ||
		   opcode == ATT_FIND_INFO_REQ || opcode == ATT_FIND_TYPE_REQ) {
		bool find_info = opcode == ATT_FIND_INFO_REQ;
		bool find_type = opcode == ATT_FIND_TYPE_REQ;
		if ((find_info && length != 4) || (find_type && length != 8 && length != 22) ||
		    (!find_info && !find_type && length != 6 && length != 20)) return -EBADMSG;
		uint16_t end = sys_get_le16(data + 2);
		if (!handle || end < handle) error = BT_ATT_ERR_INVALID_HANDLE;
		else if (opcode == ATT_READ_GROUP_REQ &&
			 (length != 6 || sys_get_le16(data + 4) != 0x2800)) {
			error = BT_ATT_ERR_UNSUPPORTED_GROUP_TYPE;
		} else {
			error = BT_ATT_ERR_ATTRIBUTE_NOT_FOUND;
			/* One result is enough for legal discovery and bounds stack/storage
			 * at the default MTU, including a 128-bit service UUID. */
			for (unsigned int s = 0; s < ARRAY_SIZE(services); s++) {
				if (!gatt->first[s]) continue;
				for (unsigned int a = 0; a < services[s].count; a++) {
					uint16_t current = gatt->first[s] + a;
					if (current < handle || current > end) continue;
					const struct attribute *attr = a ? &services[s].attributes[a - 1] : NULL;
					uint8_t type[16];
					size_t type_size = uuid_copy(type, a ? attr->uuid16 : 0x2800,
						a ? attr->uuid128 : NULL);
					if (!find_info && (type_size != (find_type ? 2 : length - 4) ||
					    memcmp(type, data + 4, type_size))) continue;
					if (find_info) {
						packet[6] = type_size == 2 ? 1 : 2;
						sys_put_le16(current, packet + 7);
						memcpy(packet + 9, type, type_size);
						return reply(packet, connection, ATT_FIND_INFO_RSP, type_size + 3);
					}
					uint8_t value[HOST_PROTOCOL_PAYLOAD_MAX - 3];
					int count = read_value(gatt, link, s, a, 0, value);
					if (count < 0) {
						error = -count;
						goto failed;
					}
					if (find_type) {
						if (a || count != length - 6 || memcmp(value, data + 6, count)) continue;
						sys_put_le16(current, packet + 6);
						sys_put_le16(gatt->first[s] + services[s].count - 1, packet + 8);
						return reply(packet, connection, ATT_FIND_TYPE_RSP, 4);
					}
					size_t prefix = opcode == ATT_READ_GROUP_REQ ? 4 : 2;
					count = MIN(count, CC2640_ATT_MTU - 2 - prefix);
					packet[6] = prefix + count;
					sys_put_le16(current, packet + 7);
					if (prefix == 4) sys_put_le16(gatt->first[s] + services[s].count - 1, packet + 9);
					memcpy(packet + 7 + prefix, value, count);
					return reply(packet, connection, opcode + 1, 1 + prefix + count);
				}
			}
		}
	} else if (opcode == ATT_PREPARE_WRITE_REQ) {
		if (length < 4) return -EBADMSG;
		error = BT_ATT_ERR_ATTRIBUTE_NOT_LONG;
	} else if (opcode == ATT_EXEC_WRITE_REQ) {
		if (length != 1) return -EBADMSG;
		if (!data[0]) return reply(packet, connection, ATT_EXEC_WRITE_RSP, 0);
	}
failed:
	return error_reply(packet, connection, opcode, handle, error);
}

int cc2640_gatt_notification(const struct cc2640_gatt *gatt, uint16_t connection,
			     enum host_ble_value value, const uint8_t *data, size_t size,
			     uint8_t packet[CC2640_GATT_PACKET_MAX])
{
	if (size > CC2640_ATT_MTU - 3 || (size && !data)) return -EMSGSIZE;
	unsigned int service, ccc;
	if (value == HOST_BLE_VALUE_PYBRICKS) { service = 3; ccc = 0; }
	else if (value == HOST_BLE_VALUE_SHELL) { service = 5; ccc = 2; }
#ifdef CONFIG_BRICKPHYR_CC2640_SMP
	else if (value == HOST_BLE_VALUE_SMP) { service = 6; ccc = 3; }
#endif
	else return -EINVAL;
	if (!gatt->first[service] || !gatt->subscribed[ccc]) return -ENOTCONN;
	/* TI ATT_HandleValueNoti adds an authentication flag before the handle.
	 * This is the reference's ATT command, not GATT_Notification (0xfd9b). */
	packet[0] = 1;
	packet[1] = 0x1b;
	packet[2] = 0xfd;
	packet[3] = size + 5;
	sys_put_le16(connection, packet + 4);
	packet[6] = 0;
	sys_put_le16(gatt->first[service] + 2, packet + 7);
	memcpy(packet + 9, data, size);
	return size + 9;
}
