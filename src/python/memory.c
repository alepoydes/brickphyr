// SPDX-License-Identifier: Apache-2.0
#include <zephyr/app_memory/app_memdomain.h>

/* Keep the descriptor privileged, outside the library's writable partition. */
K_APPMEM_PARTITION_DEFINE(python_partition);
