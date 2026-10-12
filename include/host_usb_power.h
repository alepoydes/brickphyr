// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <errno.h>

#ifdef CONFIG_BRICKPHYR_USB_BC12
/* Called only by the hub power loop, including USB-powered shutdown.
 * Returns the Pybricks source code or a negative acquisition/USB error. */
int host_usb_source_update(int vbus);
#else
static inline int host_usb_source_update(int vbus) { return vbus == 0 ? 0 : vbus < 0 ? vbus : -ENOTSUP; }
#endif
