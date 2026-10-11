// SPDX-License-Identifier: Apache-2.0
#include "native.h"
#include "hub_power.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell_dummy.h>

LOG_MODULE_REGISTER(native_test, LOG_LEVEL_INF);

int native_fixture_run(const char *name);
const void *native_fixture_hello(size_t *length);
uint32_t hub_buttons(void) { return 5; } /* Synthetic input, not a driver test. */
int storage_status(void) { return -ENOTSUP; }

K_SEM_DEFINE(stop_requested, 0, 1);
K_SEM_DEFINE(stop_completed, 0, 1);
static int stop_result;

static void terminal_stop(void *a, void *b, void *c)
{
	for (;;) {
		k_sem_take(&stop_requested, K_FOREVER);
		stop_result = shell_execute_cmd(shell_backend_dummy_get_ptr(), "app stop");
		k_sem_give(&stop_completed);
	}
}

/* Exercise the real command at the terminal's priority, not main's priority. */
K_THREAD_DEFINE(control_thread, 2048, terminal_stop, NULL, NULL, NULL,
		CONFIG_SHELL_THREAD_PRIORITY, 0, 0);

#define CHECK(condition) do { \
	if (!(condition)) { \
		printk("NATIVE FAIL line %d: %s\n", __LINE__, #condition); \
		return 1; \
	} \
} while (0)

static size_t available(void)
{
	/* Observe usable capacity through the allocator, without accessing its
	 * private state. This fixture has no other dynamic allocation clients. */
	size_t low = 0, high = CONFIG_HEAP_MEM_POOL_SIZE + 1;
	while (high - low > 1) {
		size_t length = low + (high - low) / 2;
		void *allocation = k_malloc(length);
		if (allocation) {
			low = length;
			k_free(allocation);
		} else {
			high = length;
		}
	}
	return low;
}

int main(void)
{
	LOG_WRN("Synthetic diagnostic before application unload");
	struct native_status status;
	size_t capacity = available();
	for (int i = 0; i < 8; i++) {
		int err = native_fixture_run("hello");
		printk("hello load %d\n", err);
		CHECK(err == 0);
		k_msleep(60);
		native_get_status(&status);
		printk("state %d result %d fault %u\n", status.state, status.result, status.fault);
		CHECK(status.state == NATIVE_RETURNED && status.result == 0);
		char output[80] = {0};
		CHECK(native_output(output, sizeof(output) - 1) > 0);
		CHECK(strstr(output, "Hello from a native application."));
		CHECK(status.cleanup_error == 0 && available() == capacity);
	}
	void *reservation = k_malloc(capacity);
	CHECK(reservation != NULL);
	CHECK(native_fixture_run("hello") == -ENOMEM);
	k_free(reservation);
	CHECK(available() == capacity);
	CHECK(native_fixture_run("spin") == 0);
	k_msleep(20);
	CHECK(native_fixture_run("truncated") == -ENOEXEC);
	CHECK(native_fixture_run("badabi") == -EPROTONOSUPPORT);
	native_get_status(&status);
	CHECK(status.state == NATIVE_RUNNING);
	k_sem_give(&stop_requested);
	CHECK(k_sem_take(&stop_completed, K_MSEC(100)) == 0 && stop_result == 0);
	native_get_status(&status);
	CHECK(status.state == NATIVE_STOPPED);
	CHECK(native_fixture_run("fault") == 0);
	k_msleep(20);
	native_get_status(&status);
	CHECK(status.state == NATIVE_FAULT);
	CHECK(native_fixture_run("hello") == 0);
	k_msleep(60);
	native_get_status(&status);
	CHECK(status.state == NATIVE_RETURNED && status.result == 0);
	CHECK(available() == capacity);

	/* Staging is independent of the running application and connection owner. */
	CHECK(native_fixture_run("spin") == 0);
	size_t size, next;
	const uint8_t *image = native_fixture_hello(&size);
	CHECK(native_upload(0, size, image, 128, &next) == 0 && next == 128);
	CHECK(native_upload(129, size, image + 129, 1, &next) == -EINVAL);
	uint8_t changed = image[1] ^ 1;
	CHECK(native_upload(1, size, &changed, 1, &next) == -EINVAL);
	native_get_status(&status);
	CHECK(status.upload_bytes == 128 && status.state == NATIVE_RUNNING);
	native_session_closed();
	native_poll();
	native_get_status(&status);
	CHECK(status.state == NATIVE_RUNNING && status.upload_size == 0);
	CHECK(native_run_uploaded() == -ENODATA);
	CHECK(native_upload(0, size - 1, image, size - 1, &next) == -ENOEXEC);
	native_get_status(&status);
	CHECK(status.state == NATIVE_RUNNING && status.upload_size == 0);
	for (size_t offset = 0; offset < size;) {
		size_t count = MIN(128, size - offset);
		CHECK(native_upload(offset, size, image + offset, count, &next) == 0);
		CHECK(next == offset + count);
		if (offset) {
			CHECK(native_upload(offset, size, image + offset, count, &next) == 0);
		}
		offset = next;
	}
	native_session_closed();
	native_get_status(&status);
	CHECK(status.state == NATIVE_RUNNING && status.upload_ready);
	CHECK(native_run_uploaded() == 0);
	k_msleep(60);
	native_get_status(&status);
	CHECK(status.state == NATIVE_RETURNED && status.result == 0 && status.upload_size == 0);
	CHECK(available() == capacity);
	reservation = k_malloc(capacity);
	CHECK(reservation != NULL);
	CHECK(native_upload(0, size, image, 128, &next) == -ENOMEM);
	k_free(reservation);
	CHECK(available() == capacity);

	uint8_t timeout[4];
	sys_put_le32(1, timeout);
	CHECK(settings_runtime_set("power/battery_idle_s", timeout, 4) == 0);
	int64_t now = k_uptime_get();
	CHECK(!hub_power_update(0, true, now));
	CHECK(native_fixture_run("counter") == 0);
	CHECK(!hub_power_update(0, false, now + 2000));
	CHECK(native_stop() == 0);
	CHECK(hub_power_update(0, false, k_uptime_get() + 2000));
	CHECK(native_fixture_run("hello") == -ESHUTDOWN);
	printk("NATIVE PASS: lifecycle, MPU fault, forced stop, invalid image, power\n");
	return 0;
}
