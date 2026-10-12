// SPDX-License-Identifier: Apache-2.0
#include "host_transport.h"
#include "hub_charger.h"

#include <errno.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/usb/usbd.h>

/* Experimental EP0 selector: GET -> [version, mode, occupied, ready, error32le].
 * SET wValue: 0 = Pybricks (boot default), 1 = Shell. wIndex is zero. */
#define SELECT_REQUEST 0x42
static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_shell_uart));

static bool port_open(void)
{
	uint32_t dtr = 0;
	return uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr;
}

static void usb_message(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	if (msg->type == USBD_MSG_RESET || msg->type == USBD_MSG_VBUS_REMOVED) {
		hub_charger_usb(HUB_CHARGER_USB_RESET);
	} else if (msg->type == USBD_MSG_CONFIGURATION) {
		hub_charger_usb(msg->status ? HUB_CHARGER_USB_CONFIGURED : HUB_CHARGER_USB_RESET);
	} else if (msg->type == USBD_MSG_SUSPEND) {
		hub_charger_usb(HUB_CHARGER_USB_SUSPEND);
	} else if (msg->type == USBD_MSG_RESUME) {
		hub_charger_usb(HUB_CHARGER_USB_RESUME);
	}
	if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE && msg->dev == uart) {
		host_connection_changed(port_open());
	} else if (msg->type == USBD_MSG_RESET || msg->type == USBD_MSG_VBUS_REMOVED ||
		   (msg->type == USBD_MSG_CONFIGURATION && msg->status == 0)) {
		host_connection_changed(false);
	}
}

static int selector_get(const struct usbd_context *const ctx,
			const struct usb_setup_packet *const setup, struct net_buf *const buf)
{
	if (setup->wIndex || setup->wValue || setup->wLength != 8) {
		return -ENOTSUP;
	}
	uint8_t state[8];
	host_interface_status(state);
	state[2] |= port_open();
	net_buf_add_mem(buf, state, sizeof(state));
	return 0;
}

static int selector_set(const struct usbd_context *const ctx,
			const struct usb_setup_packet *const setup, const struct net_buf *const buf)
{
	if (setup->wIndex || setup->wLength || setup->wValue > HOST_SHELL) {
		return -ENOTSUP;
	}
	/* Also inspect DTR before the asynchronous USB message has reached us. */
	return port_open() ? -EBUSY : host_interface_select(setup->wValue);
}
USBD_VREQUEST_DEFINE(selector, SELECT_REQUEST, selector_get, selector_set);

static int host_init(void)
{
	/* Factory identity location from Pybricks prime_hub_f4/pbdrvconfig.h. */
	int err = host_transport_init(uart, *(const uint16_t *)0x08007d80);
	if (err) {
		return err;
	}
	STRUCT_SECTION_FOREACH(usbd_context, ctx) {
		err = usbd_device_register_vreq(ctx, &selector);
		if (!err) {
			err = usbd_msg_register_cb(ctx, usb_message);
		}
		if (err) {
			return err;
		}
	}
	return 0;
}
/* Register control requests before the standard CDC application initializes USB. */
SYS_INIT(host_init, APPLICATION, 89);
