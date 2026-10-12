// SPDX-License-Identifier: Apache-2.0
#include "host_usb_power.h"
#include "host_transport.h"
#include "hub_charger.h"

#include <zephyr/drivers/usb/usb_bc12.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>

BUILD_ASSERT(!IS_ENABLED(CONFIG_CDC_ACM_SERIAL_ENABLE_AT_BOOT),
	"Charger detection must finish before attaching USB");

static const struct device *const detector = DEVICE_DT_GET(DT_NODELABEL(hub_usb_bc12));
static struct usbd_context *context;
static atomic_t source = ATOMIC_INIT(-EINPROGRESS);
static int initialization_error, session_error;
static bool initialized, cable, attached;

static void detected(const struct device *dev, struct bc12_partner_state *state, void *data)
{
	/* Disconnection is owned by the GPIO sampling loop. A driver callback
	 * only publishes the completed classification; it never operates USB. */
	if (!state) return;
	int type = -ENOTSUP;
	if (state->bc12_role == BC12_PORTABLE_DEVICE) {
		switch (state->type) {
		case BC12_TYPE_SDP: type = BP_CHARGER_SOURCE_SDP; break;
		case BC12_TYPE_CDP: type = BP_CHARGER_SOURCE_CDP; break;
		case BC12_TYPE_DCP: type = BP_CHARGER_SOURCE_DCP; break;
		case BC12_TYPE_UNKNOWN: type = BP_CHARGER_SOURCE_NONSTANDARD; break;
		default: break;
		}
	}
	atomic_set(&source, type);
}

int host_usb_source_update(int vbus)
{
	if (!initialized) {
		initialized = true;
		STRUCT_SECTION_FOREACH(usbd_context, ctx) {
			if (ctx->dev == DEVICE_DT_GET(DT_PARENT(DT_NODELABEL(hub_usb_bc12)))) context = ctx;
		}
		initialization_error = !context || !device_is_ready(detector) ? -ENODEV :
			bc12_set_result_cb(detector, detected, NULL);
	}
	if (initialization_error) return initialization_error;
	if (vbus != 1) {
		if (cable) {
			/* DISCONNECTED waits for an in-flight result callback. Clearing
			 * its value afterwards prevents an old result surviving a replug. */
			int err = bc12_set_role(detector, BC12_DISCONNECTED);
			int stopped = attached ? usbd_disable(context) : 0;
			if (!stopped) attached = false;
			if (!session_error) session_error = err ? err : stopped;
			hub_charger_usb(HUB_CHARGER_USB_RESET);
			host_connection_changed(false);
		}
		cable = false;
		atomic_set(&source, BP_CHARGER_SOURCE_NONE);
		return session_error ? session_error : vbus < 0 ? vbus : BP_CHARGER_SOURCE_NONE;
	}
	if (!cable) {
		cable = true;
		/* A failed detach must not start BC detection on an attached PHY. */
		if (attached) return session_error;
		session_error = 0;
		atomic_set(&source, -EINPROGRESS);
		session_error = bc12_set_role(detector, BC12_PORTABLE_DEVICE);
	}
	if (session_error) return session_error;
	int value = atomic_get(&source);
	if (value >= 0 && !attached) {
		session_error = usbd_enable(context);
		if (session_error) return session_error;
		attached = true;
	}
	return value;
}
