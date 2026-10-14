// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2023 The Pybricks Authors
/* Startup order and configuration ported from pybricks-micropython
 * a7b1b04ae4f8449dd46b3dfb97c420334439fd35,
 * lib/pbio/drv/bluetooth/bluetooth_stm32_cc2640.c:hci_init/gap_init.
 * BSD-licensed TI headers supply the protocol constants. Brickphyr encodes
 * these fixed startup commands and correlates completions,
 * validates identity/replies and preserves errors through reset cleanup.
 * See LICENSES/Pybricks-MIT.txt and LICENSES/TI-BSD-3-Clause.txt. */

#include "cc2640_controller.h"
#include "cc2640_gatt.h"
#include "cc2640_npi.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "hci_ext.h"
#include "hci_tl.h"

LOG_MODULE_REGISTER(cc2640_controller, LOG_LEVEL_ERR);

/* Fail a missing reply within two seconds. NPI separately bounds its SPI and
 * ready handshake. Startup stops on the first failed command, so this budget
 * does not multiply by the number of commands when the chip is unavailable. */
#define COMMAND_TIMEOUT K_SECONDS(2)
#define HCI_LE_MIN_DATA_OCTETS 27
#define HCI_LE_MIN_DATA_TIME_US 328

/* One startup operation owns the bounded reply storage. */
static K_MUTEX_DEFINE(controller_lock);
static const struct device *transport;
static const struct device *initialized;
static bool peripheral_ready;
static uint8_t response[CC2640_NPI_MAX_PACKET];
static size_t response_size;
static uint32_t unrelated_events;
static int first_error;
static cc2640_event_handler_t event_handler;

static uint16_t completion_event(uint16_t opcode)
{
	switch (opcode) {
	case HCI_EXT_SET_BDADDR: return 0x040c;
	case HCI_EXT_SET_TX_POWER: return 0x0401;
	case HCI_EXT_SET_MAX_DATA_LENGTH: return 0x0421;
	case HCI_EXT_SET_LOCAL_SUPPORTED_FEATURES: return 0x0406;
	case HCI_READ_BDADDR:
	case HCI_READ_LOCAL_VERSION_INFO: return HCI_EVENT_COMMAND_COMPLETE;
	case 0xfdfc: /* GATT_AddService */
	case 0xfdfe: /* GATT_AddAttribute */
	case 0xfe03: /* GAP_ConfigDeviceAddr */
	case 0xfe30: /* GAP_SetParamValue */
	case 0xfe36: /* GAP_BondMgrSetParameter */
	case 0xfe0a: /* GAP_TerminateLinkReq: completion is the separate link event. */
		return HCI_COMMAND_STATUS;
	case 0xfe00: return 0x0600; /* GAP_DeviceInit / GAP_DeviceInitDone */
	case 0xfe07: return 0x0602; /* GAP_UpdateAdvertisingData */
	case 0xfe06: return 0x0603; /* GAP_MakeDiscoverable */
	default:
		/* ATT response/notification commands have the ATT opcode in the low
		 * byte. The network processor acknowledges their queue admission. */
		return (opcode & 0xffe0) == 0xfd00 && (opcode & 1) ? HCI_COMMAND_STATUS : 0;
	}
}

static int command_status(uint16_t opcode, uint8_t status)
{
	if (!status) return 0;
	if ((opcode & 0xffe0) == 0xfd00) {
		if (status == blePending || status == bleMsgBufferNotAvailable ||
		    status == bleMemAllocError || status == bleNoResources) return -EAGAIN;
		if (status == bleNotConnected) return -ENOTCONN;
	}
	if (opcode == 0xfe0a && (status == bleIncorrectMode || status == bleNotConnected)) {
		return -ENOTCONN;
	}
	LOG_ERR("Command 0x%04x status 0x%02x", opcode, status);
	return -EIO;
}

/* A positive result completes this command; zero is an unrelated event or an
 * intermediate success status. A negative result is a malformed/error reply.
 * Runtime callers dispatch non-completing events before receiving more data. */
static int match_reply(uint16_t opcode, uint16_t expected)
{
	if (response_size < 3 || response[0] != HCI_EVENT_PACKET ||
	    response[2] != response_size - 3) return -EBADMSG;
	if (response[1] == HCI_EVENT_COMMAND_COMPLETE) {
		if (response_size < 7) return -EBADMSG;
		if (sys_get_le16(response + 4) != opcode) return 0;
		int err = command_status(opcode, response[6]);
		if (err) return err;
		return expected == HCI_EVENT_COMMAND_COMPLETE ? 1 : 0;
	}
	if (response[1] != HCI_EVENT_VENDOR_SPECIFIC) return 0;
	if (response_size < 6) return -EBADMSG;
	uint16_t event = sys_get_le16(response + 3);
	if (event == HCI_COMMAND_STATUS) {
		if (response_size < 9 || response[8] != response_size - 9) return -EBADMSG;
		if (sys_get_le16(response + 6) != opcode) return 0;
		/* Some commands complete here; others still require their separate
		 * result event after this receipt acknowledgment. */
		int err = command_status(opcode, response[5]);
		return err ? err : expected == HCI_COMMAND_STATUS ? 1 : 0;
	}
	if (event != expected) return 0;
	int err = command_status(opcode, response[5]);
	return err ? err : 1;
}

static int send_command(uint16_t opcode, const uint8_t *payload, uint8_t length)
{
	if (first_error) return first_error;
	uint16_t expected = completion_event(opcode);
	int err = !expected ? -ENOTSUP : length > CC2640_NPI_MAX_PACKET - 4 ||
		(length && !payload) ? -EINVAL : 0;
	if (err) goto failed;
	/* NPI consumes TX before publishing RX. The previous reply has already
	 * been consumed, so command staging can reuse its storage. */
	if (length) memcpy(response + 4, payload, length);
	response[0] = HCI_CMD_PACKET;
	sys_put_le16(opcode, response + 1);
	response[3] = length;
	k_timepoint_t deadline = sys_timepoint_calc(COMMAND_TIMEOUT);
	err = cc2640_npi_exchange(transport, response, length + 4, response, K_NO_WAIT);
	if (err < 0) goto failed;
	for (;;) {
		response_size = err > 0 ? err : 0;
		if (response_size) {
			err = match_reply(opcode, expected);
			if (err < 0) goto failed;
			if (err > 0) return 0;
			unrelated_events++;
			if (event_handler) {
				err = event_handler(response, response_size);
				if (err) goto failed;
			}
		}
		if (sys_timepoint_expired(deadline)) {
			err = -ETIMEDOUT;
			goto failed;
		}
		err = cc2640_npi_exchange(transport, NULL, 0, response,
					  sys_timepoint_timeout(deadline));
		if (err == -EAGAIN) err = -ETIMEDOUT;
		if (err < 0) goto failed;
	}
failed:
	if (err == -EAGAIN || err == -ENOTCONN) return err;
	first_error = err;
	LOG_ERR("Command 0x%04x: %d", opcode, err);
	return err;
}

int cc2640_controller_start(const struct device *npi, const uint8_t address[6],
			    struct cc2640_info *info)
{
	if (!npi || !address || !info) return -EINVAL;
	/* Erased or missing factory identity must not become a shared address. */
	static const uint8_t zero[6], erased[6] = {255, 255, 255, 255, 255, 255};
	if (!memcmp(address, zero, 6) || !memcmp(address, erased, 6)) return -EINVAL;
	if (k_mutex_lock(&controller_lock, K_NO_WAIT)) return -EBUSY;
	transport = npi;
	initialized = NULL;
	peripheral_ready = false;
	first_error = cc2640_npi_reset(npi);
	response_size = unrelated_events = 0;
	struct cc2640_info observed = {0};
	if (first_error) goto done;
	if (send_command(HCI_EXT_SET_BDADDR, address, 6) ||
	    send_command(HCI_READ_BDADDR, NULL, 0)) goto done;
	if (response_size != 13 || memcmp(response + 7, address, 6)) {
		first_error = -EBADMSG;
		LOG_ERR("Controller address verification: %d", first_error);
		goto done;
	}
	memcpy(observed.address, response + 7, 6);
	const uint8_t power = HCI_EXT_CC26XX_TX_POWER_0_DBM;
	if (send_command(HCI_EXT_SET_TX_POWER, &power, sizeof(power)) ||
	    send_command(HCI_READ_LOCAL_VERSION_INFO, NULL, 0)) goto done;
	if (response_size != 15) {
		first_error = -EBADMSG;
		LOG_ERR("Controller version reply: %d", first_error);
		goto done;
	}
	observed.revision = sys_get_le16(response + 8);
	observed.manufacturer = sys_get_le16(response + 11);
	/* Retain the reference order: after setting features the controller can
	 * refuse the packet limit yet still accept packets too large for its RAM. */
	uint8_t limit[8], features[8] = {HCI_EXT_LOCAL_FEATURE_ENCRYTION};
	sys_put_le16(HCI_LE_MIN_DATA_OCTETS, limit);
	sys_put_le16(HCI_LE_MIN_DATA_TIME_US, limit + 2);
	memcpy(limit + 4, limit, 4);
	if (send_command(HCI_EXT_SET_MAX_DATA_LENGTH, limit, sizeof(limit)) ||
	    send_command(HCI_EXT_SET_LOCAL_SUPPORTED_FEATURES, features, sizeof(features))) goto done;
	observed.unrelated_events = unrelated_events;
	*info = observed;
	initialized = npi;
done:
	if (first_error) (void)cc2640_npi_off(npi);
	int err = first_error;
	transport = NULL;
	k_mutex_unlock(&controller_lock);
	return err;
}

static int configure_peripheral(void)
{
	/* The management endpoint does not pair. Unlike the reference's central
	 * support, it needs neither bond erasure nor any scanning configuration. */
	const uint8_t no_pairing[] = {0x00, 0x04, 1, 0};
	int err = send_command(0xfe36, no_pairing, sizeof(no_pairing));
	if (err) return err;
	/* Reference general/connectable advertising min/max: 40 * 0.625 ms.
	 * Configuration alone does not make the controller discoverable. */
	for (uint8_t id = 8; id <= 11; id++) {
		const uint8_t interval[] = {id, 40, 0};
		err = send_command(0xfe30, interval, sizeof(interval));
		if (err) return err;
	}
	/* Peripheral role only; retain the reference scan-response capacity and
	 * zero IRK/CSRK/counter. A status ACK is not the DeviceInitDone event. */
	const uint8_t init[38] = {0x04, 8};
	err = send_command(0xfe00, init, sizeof(init));
	if (err) return err;
	/* Explicit public identity, already set and read back during HCI startup.
	 * This matches the F4 management endpoint; no replacement address is stored. */
	const uint8_t address[7] = {0};
	return send_command(0xfe03, address, sizeof(address));
}

int cc2640_controller_peripheral(struct cc2640_gatt *gatt)
{
	if (!gatt) return -EINVAL;
	if (k_mutex_lock(&controller_lock, K_NO_WAIT)) return -EBUSY;
	int err = !initialized ? -ENODEV : peripheral_ready ? -EALREADY : 0;
	if (err) goto done;
	transport = initialized;
	struct cc2640_gatt registered = {0};
	uint8_t command[CC2640_GATT_PACKET_MAX];
	for (unsigned int s = 0; s < CC2640_GATT_SERVICE_COUNT; s++) {
		/* Preserve the reference order: Generic Attribute/Access before GAP
		 * initialization, Device Information and application services after it. */
		if (s == 2) {
			err = configure_peripheral();
			if (err) goto failed;
		}
		/* The reply to the last attribute (or a one-attribute service itself)
		 * carries its allocated handle range, as in the reference adapter. */
		for (unsigned int a = 0; cc2640_gatt_registration(s, a, command); a++) {
			err = send_command(sys_get_le16(command + 1), command + 4, command[3]);
			if (err) goto failed;
		}
		if (response_size != 13 || response[8] != 4) {
			err = -EBADMSG;
			goto failed;
		}
		err = cc2640_gatt_bind(&registered, s, sys_get_le16(response + 9),
				       sys_get_le16(response + 11));
		if (err) goto failed;
	}
	*gatt = registered;
	peripheral_ready = true;
	goto done;
failed:
	if (!first_error) {
		first_error = err;
		LOG_ERR("Peripheral registration: %d", err);
	}
	(void)cc2640_npi_off(initialized);
	initialized = NULL;
done:
	transport = NULL;
	k_mutex_unlock(&controller_lock);
	return err;
}

int cc2640_controller_command(const uint8_t *packet, size_t size,
			      cc2640_event_handler_t receive)
{
	if (!packet || size < 4 || size > CC2640_NPI_MAX_PACKET || packet[0] != 1 ||
	    packet[3] != size - 4 || !receive) return -EINVAL;
	if (k_mutex_lock(&controller_lock, K_NO_WAIT)) return -EBUSY;
	int err = first_error ? first_error : !peripheral_ready ? -EHOSTDOWN : 0;
	if (!err) {
		transport = initialized;
		event_handler = receive;
		err = send_command(sys_get_le16(packet + 1), packet + 4, packet[3]);
		event_handler = NULL;
		transport = NULL;
	}
	k_mutex_unlock(&controller_lock);
	return err;
}

int cc2640_controller_poll(cc2640_event_handler_t receive)
{
	if (!receive) return -EINVAL;
	if (k_mutex_lock(&controller_lock, K_NO_WAIT)) return -EBUSY;
	int err = first_error ? first_error : !peripheral_ready ? -EHOSTDOWN : 0;
	if (!err) {
		err = cc2640_npi_exchange(initialized, NULL, 0, response, K_NO_WAIT);
		if (err == -EAGAIN) err = 0;
		else if (err > 0) {
			err = receive(response, err);
			if (!err) err = 1;
		}
		if (err < 0) first_error = err;
	}
	k_mutex_unlock(&controller_lock);
	return err;
}

int cc2640_controller_stop(void)
{
	if (k_mutex_lock(&controller_lock, K_NO_WAIT)) return -EBUSY;
	int err = initialized ? cc2640_npi_off(initialized) : 0;
	if (!err) {
		initialized = NULL;
		peripheral_ready = false;
	}
	k_mutex_unlock(&controller_lock);
	return err;
}
