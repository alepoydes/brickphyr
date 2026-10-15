// SPDX-License-Identifier: Apache-2.0
#include "storage_work.h"
#include <zephyr/init.h>
#include <zephyr/kernel.h>

/* Reuse the Pybricks slot writer's filesystem-call budget. An ARM LittleFS
 * check exhausted 2 KiB; sharing this queue avoids another stack. */
static struct k_work_q queue;
static K_KERNEL_STACK_DEFINE(stack, 4096);

int storage_work_submit(struct k_work *work)
{
	return k_work_submit_to_queue(&queue, work);
}

static int initialize(void)
{
	const struct k_work_queue_config config = {.name = "app_store"};
	k_work_queue_start(&queue, stack, K_KERNEL_STACK_SIZEOF(stack),
		K_LOWEST_APPLICATION_THREAD_PRIO - 1, &config);
	return 0;
}
SYS_INIT(initialize, APPLICATION, 90);
