// SPDX-License-Identifier: Apache-2.0
#include "configuration.h"
#include "storage.h"
#include "storage_work.h"

#include <limits.h>
#include <zephyr/kernel.h>

static struct k_spinlock save_lock;
static uint32_t save_request;
static int save_result;

static void save_worker(struct k_work *work)
{
	/* Settings handlers own validation and selected values. Read them here:
	 * no application holds their locks while it waits for flash operations. */
	int err = config_save();
	k_spinlock_key_t key = k_spin_lock(&save_lock);
	save_result = err;
	k_spin_unlock(&save_lock, key);
}
static K_WORK_DEFINE(save_work, save_worker);

int config_save_start(void)
{
	int err = storage_status();
	if (err) return err;
	k_spinlock_key_t key = k_spin_lock(&save_lock);
	err = save_result == -EINPROGRESS ? -EBUSY :
		save_request == INT_MAX ? -EOVERFLOW : 0;
	if (!err) {
		/* Publish and submit together: aborting the caller cannot strand a
		 * pending request. The work retains no application memory. */
		err = storage_work_submit(&save_work);
		if (err >= 0) {
			save_result = -EINPROGRESS;
			err = ++save_request;
		}
	}
	k_spin_unlock(&save_lock, key);
	return err;
}

int config_save_status(uint32_t request)
{
	k_spinlock_key_t key = k_spin_lock(&save_lock);
	int result = !request || request > INT_MAX ? -EINVAL :
		request != save_request ? -ESTALE : save_result;
	k_spin_unlock(&save_lock, key);
	return result;
}

int config_save_finish(void)
{
	/* Called after application/management admission is closed. */
	struct k_work_sync sync;
	(void)k_work_flush(&save_work, &sync);
	k_spinlock_key_t key = k_spin_lock(&save_lock);
	int err = save_result;
	k_spin_unlock(&save_lock, key);
	return err;
}
