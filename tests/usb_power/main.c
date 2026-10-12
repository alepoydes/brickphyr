// SPDX-License-Identifier: Apache-2.0
/* Synthetic BC1.2 and USB attachment around the production power coordinator. */
#include "host_usb_power.h"
#include "host_transport.h"
#include "hub_charger.h"

#include <zephyr/device.h>
#include <zephyr/drivers/usb/usb_bc12.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/ztest.h>

static bc12_callback_t result_callback;
static void *result_data;
static enum bc12_role role = BC12_DISCONNECTED;
static unsigned int starts, cancels, enables, disables, resets, host_disconnects;
static int enable_error, disable_error;

static int fake_set_role(const struct device *dev, enum bc12_role next)
{
	zassert_equal(dev, DEVICE_DT_GET(DT_NODELABEL(hub_usb_bc12)));
	if (next == BC12_PORTABLE_DEVICE) {
		zassert_equal(role, BC12_DISCONNECTED);
		starts++;
	} else {
		zassert_equal(next, BC12_DISCONNECTED);
		cancels++;
	}
	/* Cancellation retires old results; the callback remains registered. */
	role = next;
	return 0;
}

static int fake_set_result_cb(const struct device *dev, bc12_callback_t cb, void *data)
{
	zassert_equal(dev, DEVICE_DT_GET(DT_NODELABEL(hub_usb_bc12)));
	zassert_is_null(result_callback);
	result_callback = cb;
	result_data = data;
	return 0;
}

static DEVICE_API(bc12, bc12_api) = {
	.set_role = fake_set_role,
	.set_result_cb = fake_set_result_cb,
};
DEVICE_DT_DEFINE(DT_NODELABEL(test_udc), NULL, NULL, NULL, NULL,
	POST_KERNEL, 90, NULL);
DEVICE_DT_DEFINE(DT_NODELABEL(hub_usb_bc12), NULL, NULL, NULL, NULL,
	POST_KERNEL, 90, &bc12_api);

USBD_DEVICE_DEFINE(test_usbd, DEVICE_DT_GET(DT_NODELABEL(test_udc)), 0x2fe3, 0xffff);

int __wrap_usbd_enable(struct usbd_context *ctx)
{
	zassert_equal(ctx, &test_usbd);
	enables++;
	return enable_error;
}

int __wrap_usbd_disable(struct usbd_context *ctx)
{
	zassert_equal(ctx, &test_usbd);
	disables++;
	return disable_error;
}

void hub_charger_usb(enum hub_charger_usb_event event)
{
	zassert_equal(event, HUB_CHARGER_USB_RESET);
	resets++;
}

void host_connection_changed(bool open)
{
	zassert_false(open);
	host_disconnects++;
}

static bool deliver(enum bc12_type type)
{
	if (role != BC12_PORTABLE_DEVICE) return false;
	zassert_not_null(result_callback);
	struct bc12_partner_state result = {
		.bc12_role = BC12_PORTABLE_DEVICE,
		.type = type,
	};
	result_callback(DEVICE_DT_GET(DT_NODELABEL(hub_usb_bc12)), &result, result_data);
	return true;
}

ZTEST(usb_power, coordinator_lifecycle)
{
	/* A cold VBUS report must start detection without attaching USB. */
	zassert_equal(host_usb_source_update(1), -EINPROGRESS);
	zassert_equal(starts, 1);
	zassert_equal(enables, 0);
	zassert_equal(host_usb_source_update(1), -EINPROGRESS);
	zassert_equal(starts, 1);
	zassert_true(deliver(BC12_TYPE_SDP));
	zassert_equal(host_usb_source_update(1), BP_CHARGER_SOURCE_SDP);
	zassert_equal(enables, 1);
	zassert_equal(host_usb_source_update(1), BP_CHARGER_SOURCE_SDP);
	zassert_equal(enables, 1);

	zassert_equal(host_usb_source_update(0), BP_CHARGER_SOURCE_NONE);
	zassert_equal(disables, 1);
	zassert_equal(resets, 1);
	zassert_equal(host_disconnects, 1);
	zassert_equal(host_usb_source_update(0), BP_CHARGER_SOURCE_NONE);
	zassert_equal(resets, 1);

	/* Zephyr numbers DCP before CDP; the SDK mapping must be explicit. */
	zassert_equal(host_usb_source_update(1), -EINPROGRESS);
	zassert_true(deliver(BC12_TYPE_CDP));
	zassert_equal(host_usb_source_update(1), BP_CHARGER_SOURCE_CDP);
	zassert_equal(host_usb_source_update(0), BP_CHARGER_SOURCE_NONE);
	zassert_equal(host_usb_source_update(1), -EINPROGRESS);
	zassert_true(deliver(BC12_TYPE_DCP));
	zassert_equal(host_usb_source_update(1), BP_CHARGER_SOURCE_DCP);
	zassert_equal(host_usb_source_update(0), BP_CHARGER_SOURCE_NONE);
	zassert_equal(enables, 3);
	zassert_equal(disables, 3);

	/* Pulling VBUS during detection cancels it; no old class may attach. */
	zassert_equal(host_usb_source_update(1), -EINPROGRESS);
	zassert_equal(host_usb_source_update(0), BP_CHARGER_SOURCE_NONE);
	zassert_false(deliver(BC12_TYPE_DCP));
	zassert_equal(enables, 3);
	zassert_equal(host_usb_source_update(1), -EINPROGRESS);
	zassert_equal(enables, 3);
	zassert_true(deliver(BC12_TYPE_UNKNOWN));
	zassert_equal(host_usb_source_update(1), BP_CHARGER_SOURCE_NONSTANDARD);
	zassert_equal(host_usb_source_update(0), BP_CHARGER_SOURCE_NONE);

	/* USB failure is stable for one cable session and clears on replug. */
	enable_error = -EIO;
	zassert_equal(host_usb_source_update(1), -EINPROGRESS);
	zassert_true(deliver(BC12_TYPE_SDP));
	zassert_equal(host_usb_source_update(1), -EIO);
	unsigned int attempted = enables;
	zassert_equal(host_usb_source_update(1), -EIO);
	zassert_equal(enables, attempted);
	zassert_equal(host_usb_source_update(0), -EIO);
	zassert_equal(resets, host_disconnects);
	enable_error = 0;
	zassert_equal(host_usb_source_update(1), -EINPROGRESS);
	zassert_true(deliver(BC12_TYPE_SDP));
	zassert_equal(host_usb_source_update(1), BP_CHARGER_SOURCE_SDP);
	zassert_equal(enables, attempted + 1);
	zassert_equal(host_usb_source_update(0), BP_CHARGER_SOURCE_NONE);
	zassert_equal(resets, host_disconnects);
	zassert_equal(cancels, starts);

	/* A failed detach leaves the PHY state unknown. Do not poll-retry it
	 * or begin another D+/D- measurement while it may still be attached. */
	zassert_equal(host_usb_source_update(1), -EINPROGRESS);
	zassert_true(deliver(BC12_TYPE_SDP));
	zassert_equal(host_usb_source_update(1), BP_CHARGER_SOURCE_SDP);
	disable_error = -EIO;
	zassert_equal(host_usb_source_update(0), -EIO);
	attempted = disables;
	zassert_equal(host_usb_source_update(0), -EIO);
	zassert_equal(disables, attempted);
	attempted = starts;
	zassert_equal(host_usb_source_update(1), -EIO);
	zassert_equal(starts, attempted);
}

ZTEST_SUITE(usb_power, NULL, NULL, NULL, NULL, NULL);
