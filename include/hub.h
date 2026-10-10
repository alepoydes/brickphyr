// SPDX-License-Identifier: Apache-2.0
#ifndef BRICKPHYR_HUB_H_
#define BRICKPHYR_HUB_H_

/* Privileged F4 board operations; not an application ABI. */
int hub_reboot_dfu(void);
void hub_shutdown(void);
void hub_run(void);

#endif
