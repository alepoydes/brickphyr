// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host_ble_session.h"
#include <stddef.h>
#include <stdint.h>

#define CC2640_ATT_MTU 23
#define CC2640_GATT_PACKET_MAX (CC2640_ATT_MTU + 6)
#define CC2640_GATT_SERVICE_COUNT 6

/* One controller database and one accepted management link. Clear subscriptions
 * at each link boundary; retain the handles until the controller is reset. */
struct cc2640_gatt {
	uint16_t first[CC2640_GATT_SERVICE_COUNT];
	uint16_t subscribed[3]; /* Pybricks, legacy NUS, native Shell. */
};

/* Service and attribute indices begin at zero. Attribute zero registers the
 * primary service; subsequent indices register its attributes. Returns the HCI
 * command size, or zero when that service has no more attributes. */
size_t cc2640_gatt_registration(unsigned int service, unsigned int attribute,
			       uint8_t packet[CC2640_GATT_PACKET_MAX]);
/* Publish the handles returned after the service's last attribute. Reject
 * missing, overlapping or incorrectly sized ranges before replacing state. */
int cc2640_gatt_bind(struct cc2640_gatt *gatt, unsigned int service,
		    uint16_t first, uint16_t last);

/* Decode one TI ATT event for the accepted connection and build its HCI reply.
 * The worker serializes calls and sends returned bytes before accepting another
 * ATT request. Positive: reply size; zero: no response (write command); negative:
 * malformed/unrelated event, with no request side effects. The link uses the
 * default ATT MTU in both directions. No controller I/O occurs here. */
int cc2640_gatt_request(struct cc2640_gatt *gatt, struct host_ble_link *link,
		       uint16_t connection, const uint8_t *event, size_t size,
		       uint8_t packet[CC2640_GATT_PACKET_MAX]);
