// SPDX-License-Identifier: Apache-2.0
#pragma once

struct k_work;

/* Shared filesystem worker, below control and hardware service priorities.
 * Work and its inputs remain owned by the firmware through completion. */
int storage_work_submit(struct k_work *work);
