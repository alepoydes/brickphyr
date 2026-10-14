// SPDX-License-Identifier: Apache-2.0
#include "application.h"
#include "hub_power.h"
#include "hub_matrix.h"
#include "hub_light.h"
#include "hub_sound.h"
#include "hub_battery.h"
#include "hub_ports.h"
#include "../ports/wiring.h"

extern bool sound_output_active;

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/fff.h>

DEFINE_FFF_GLOBALS;

static bool hold_transfer;
static uint8_t led_frame[97];
K_SEM_DEFINE(transfer_started, 0, 1);
K_SEM_DEFINE(transfer_continue, 0, 1);

static int receive_frame(const struct emul *emul, const struct spi_config *cfg,
			 const struct spi_buf_set *tx, const struct spi_buf_set *rx)
{
	if (hold_transfer) {
		hold_transfer = false;
		k_sem_give(&transfer_started);
		if (k_sem_take(&transfer_continue, K_MSEC(500))) {
			return -ETIMEDOUT;
		}
	}
	if (tx->count != 1 || tx->buffers[0].len != sizeof(led_frame)) {
		return -EINVAL;
	}
	memcpy(led_frame, tx->buffers[0].buf, sizeof(led_frame));
	return 0;
}

static int emul_init(const struct emul *emul, const struct device *bus) { return 0; }
static const struct spi_emul_api spi_api = {.io = receive_frame};
EMUL_DT_DEFINE(DT_NODELABEL(hub_leds), emul_init, NULL, NULL, &spi_api, NULL);

LOG_MODULE_REGISTER(native_test, LOG_LEVEL_INF);

int native_fixture_run(const char *name);
int native_diagnostics_check(void);
const void *native_fixture_hello(size_t *length);
#ifdef NATIVE_CAPACITY_TEST
const void *native_fixture_capacity(size_t *length);
#endif
static uint32_t test_buttons = 5;
uint32_t hub_buttons(void) { return test_buttons; } /* Synthetic input. */
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

#ifdef CONFIG_FPU
static int check_fp_context(void)
{
	CHECK(native_fixture_run("fpu") == 0);
	uint32_t expected[16], observed[16];
	for (unsigned int i = 0; i < ARRAY_SIZE(expected); i++) {
		expected[i] = 0x42000000U + (i << 12);
	}
	for (unsigned int pass = 0; pass < 12; pass++) {
		__asm__ volatile("vldmia %0, {s16-s31}" : : "r"(expected) :
			"s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
			"s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31", "memory");
		k_msleep(1);
		__asm__ volatile("vstmia %0, {s16-s31}" : : "r"(observed) : "memory");
		CHECK(!memcmp(expected, observed, sizeof(expected)));
	}
	struct application_status status;
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RETURNED && status.result == 0 && !status.cleanup_error);
	return 0;
}
#endif

int main(void)
{
	size_t capacity = available();
	/* Cold entry must work with dirty backing obtained from the real heap. */
	void *dirty = k_malloc(capacity);
	CHECK(dirty != NULL);
	memset(dirty, 0xa5, capacity);
	k_free(dirty);
	LOG_WRN("Synthetic diagnostic before application unload");
	struct application_status status;
#ifdef NATIVE_CAPACITY_TEST
	/* Load a real 64 KiB user BSS into dirty backing before other applications
	 * can establish prerequisites. The maximum upload must coexist with it. */
	size_t large_size, large_next;
	const void *large = native_fixture_capacity(&large_size);
	CHECK(application_upload(APPLICATION_NATIVE, 0, large_size, large, large_size, &large_next) == 0);
	CHECK(large_next == large_size);
	CHECK(application_run_uploaded(false) == 0);
	int64_t capacity_deadline = k_uptime_get() + 500;
	do {
		k_sleep(K_TICKS(1));
		application_get_status(&status);
		CHECK(k_uptime_get() < capacity_deadline);
	} while (status.state == APPLICATION_RUNNING);
	CHECK(status.state == APPLICATION_RETURNED && !status.result && !status.cleanup_error);
	CHECK(!status.upload_size);
	CHECK(available() == capacity);
	printk("Native capacity: image %zu, loader %zu, allocated %zu bytes\n",
		status.image_bytes, status.loader_bytes, status.loaded_bytes);
#endif
	CHECK(native_diagnostics_check() == 0);
#ifdef CONFIG_FPU
	CHECK(check_fp_context() == 0 && available() == capacity);
#endif
#ifdef CONFIG_CMSIS_DSP_FILTERING
	int dsp_error = native_fixture_run("dsp");
	printk("DSP load %d\n", dsp_error);
	CHECK(dsp_error == 0);
	k_msleep(20);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RETURNED && status.result == 0);
	CHECK(!status.fault && !status.cleanup_error && available() == capacity);
#endif
	size_t hello_budget = 0;
	for (int i = 0; i < (CONFIG_BRICKPHYR_APPLICATION_STACK_SIZE >= 4096 ? 3 : 2); i++) {
		int err = native_fixture_run(i == 2 ? "largerstack" : i ? "metadata" : "hello");
		printk("hello load %d\n", err);
		CHECK(err == 0);
		k_msleep(60);
		/* Observation must not unload a returned app or consume its output. */
		CHECK(application_observe(&status) == 0 && status.output_bytes > 0);
		size_t retained = available();
		CHECK(status.state == APPLICATION_RUNNING && status.loaded_bytes > 0);
		CHECK(application_observe(&status) == 0 && available() == retained);
		application_get_status(&status);
		printk("state %d result %d fault %u\n", status.state, status.result, status.fault);
		CHECK(status.state == APPLICATION_RETURNED && status.result == 0);
		CHECK(status.stack_bytes == (i == 2 ? 4096 : 2048));
		char output[80] = {0};
		CHECK(application_output(output, sizeof(output) - 1) > 0);
		CHECK(strstr(output, "Hello from a native application."));
		CHECK(status.cleanup_error == 0 && available() == capacity);
		if (!i) {
			hello_budget = status.loader_bytes;
		} else {
			CHECK(status.loader_bytes == hello_budget);
		}
	}
	/* A replacement must secure its arena before stopping the old image. */
	CHECK(native_fixture_run("spin") == 0);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RUNNING);
	size_t running_capacity = available();
	void *reservation = k_malloc(running_capacity);
	CHECK(reservation != NULL);
	CHECK(available() < hello_budget);
	CHECK(native_fixture_run("hello") == -ENOMEM);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RUNNING && !status.cleanup_error);
	k_free(reservation);
	CHECK(available() == running_capacity);
	/* The new loader fits, but its protected stack does not. Admission must
	 * still preserve the running thread and reclaim its partial reservation. */
	reservation = k_malloc(running_capacity - hello_budget - 256);
	CHECK(reservation != NULL && available() >= hello_budget);
	CHECK(native_fixture_run("hello") == -ENOMEM);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RUNNING && !status.cleanup_error);
	k_free(reservation);
	CHECK(available() == running_capacity);
	CHECK(application_stop() == 0 && available() == capacity);
	CHECK(native_fixture_run("hello") == 0);
	k_msleep(60);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RETURNED && status.result == 0 &&
	      !status.cleanup_error && available() == capacity);

	/* Exercise the new ABI from a separately loaded userspace image. This
	 * board lacks an IMU: the real call must report ENOTSUP, without a fault. */
	CHECK(native_fixture_run("imu") == 0);
	k_msleep(20);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RETURNED && status.result == -ENOTSUP);
	CHECK(status.cleanup_error == 0 && available() == capacity);

	CHECK(native_fixture_run("sound") == 0);
	/* Tone and PCM consume fewer than 80 halves in this tick-driven sink. */
	int64_t deadline = k_uptime_get() + k_ticks_to_ms_ceil64(100);
	do {
		k_sleep(K_TICKS(1));
		application_get_status(&status);
		CHECK(k_uptime_get() < deadline);
	} while (status.state == APPLICATION_RUNNING);
	printk("sound: state %d result %d fault %u cleanup %d\n", status.state,
	       status.result, status.fault, status.cleanup_error);
	CHECK(status.state == APPLICATION_RETURNED && status.result == 0);
	struct bp_sound_state speaker;
	CHECK(hub_sound_get(&speaker) == 0 && speaker.completed_samples == 8192);
	CHECK(speaker.owner == BP_SOUND_FREE && !sound_output_active);
	CHECK(status.cleanup_error == 0 && available() == capacity);
	CHECK(native_fixture_run("sound") == 0);
	k_msleep(10);
	CHECK(sound_output_active);
	CHECK(application_stop() == 0);
	CHECK(!sound_output_active);
	CHECK(hub_sound_get(&speaker) == 0 && speaker.owner == BP_SOUND_FREE);
	CHECK(available() == capacity);
	CHECK(native_fixture_run("sound_fault") == 0);
	k_msleep(20);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_FAULT && status.cleanup_error == 0);
	CHECK(hub_sound_get(&speaker) == 0 && speaker.owner == BP_SOUND_FREE);
	CHECK(!sound_output_active && available() == capacity);
	/* A failed driver release must retain the loaded image until retry. */
	CHECK(shell_execute_cmd(shell_backend_dummy_get_ptr(), "sound_error 0 1") == 0);
	CHECK(native_fixture_run("sound") == 0);
	k_msleep(10);
	CHECK(sound_output_active);
	CHECK(application_stop() == -EACCES);
	CHECK(hub_sound_get(&speaker) == 0 && speaker.owner == BP_SOUND_APPLICATION);
	size_t held_capacity = available();
	CHECK(held_capacity < capacity);
	CHECK(native_fixture_run("spin") == -EACCES);
	CHECK(available() == held_capacity);
	CHECK(shell_execute_cmd(shell_backend_dummy_get_ptr(), "sound_error 0 0") == 0);
	CHECK(application_stop() == 0);
	CHECK(hub_sound_get(&speaker) == 0 && speaker.owner == BP_SOUND_FREE);
	CHECK(available() == capacity);
	CHECK(native_fixture_run("battery_fault") == 0);
	deadline = k_uptime_get() + 1600;
	do {
		k_msleep(10);
		application_get_status(&status);
		CHECK(k_uptime_get() < deadline);
	} while (status.state == APPLICATION_RUNNING);
	CHECK(status.state == APPLICATION_FAULT && status.cleanup_error == 0);
	for (test_buttons = 0; test_buttons < 2; test_buttons++) {
		CHECK(native_fixture_run("system_fault") == 0);
		k_msleep(20);
		application_get_status(&status);
		CHECK(status.state == APPLICATION_FAULT && status.cleanup_error == 0);
	}
	test_buttons = 5;
	CHECK(native_fixture_run("unsupported") == 0);
	k_msleep(20);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RETURNED && status.result == 0 && status.cleanup_error == 0);
	struct bp_battery_sample battery;
	CHECK(hub_battery_get(&battery) == 0 && battery.error == 0);
	CHECK(available() == capacity);

	pup_test_circuits[0].kind = MEDIUM_MOTOR;
	struct bp_motor_state motor;
	/* Twenty-one confirmations, each at most ten settling intervals. This
	 * QEMU fixture uses 100 Hz ticks; account for timeout tick rounding. */
	deadline = k_uptime_get() + 22 * 10 * (2 + k_ticks_to_ms_ceil64(2));
	do {
		k_msleep(1);
		CHECK(hub_motor_get(0, &motor) == 0);
		CHECK(k_uptime_get() < deadline);
	} while (motor.error);
	for (test_buttons = 0; test_buttons < 3; test_buttons++) {
		CHECK(native_fixture_run("motor") == 0);
		deadline = k_uptime_get() + 500;
		do {
			k_msleep(1);
			CHECK(hub_motor_get(0, &motor) == 0);
			application_get_status(&status);
			CHECK(k_uptime_get() < deadline);
		} while (status.state == APPLICATION_RUNNING && (test_buttons != 1 || !motor.duty));
		if (test_buttons == 1) {
			CHECK(status.state == APPLICATION_RUNNING && motor.owner == BP_PORT_APPLICATION);
			CHECK(application_stop() == 0);
		} else {
			CHECK(status.state == (test_buttons == 0 ? APPLICATION_RETURNED : APPLICATION_FAULT));
			CHECK(status.result == 0 && status.cleanup_error == 0);
		}
		CHECK(hub_motor_get(0, &motor) == 0 && motor.owner == BP_PORT_UNOWNED);
		CHECK(motor.valid && motor.mode == BP_MOTOR_COAST);
		CHECK(pup_test_sensor_supply(0) == 0 && available() == capacity);
	}
	test_buttons = 5;

	/* Real userspace syscalls and production driver/service with a synthetic SPI
	 * peer. Stop must join the app and wait for the outstanding final blank. */
	hold_transfer = true;
	CHECK(native_fixture_run("matrix") == 0);
	CHECK(k_sem_take(&transfer_started, K_MSEC(100)) == 0);
	k_sem_give(&stop_requested);
	CHECK(k_sem_take(&stop_completed, K_MSEC(10)) == -EAGAIN);
	CHECK(application_observe(&status) == -EBUSY);
	k_sem_give(&transfer_continue);
	CHECK(k_sem_take(&stop_completed, K_MSEC(100)) == 0 && stop_result == 0);
	struct bp_matrix_state display;
	CHECK(hub_matrix_get(&display) == 0);
	CHECK(display.owner == BP_MATRIX_FREE && !display.pending && display.valid);
	CHECK(led_frame[73] == 0 && led_frame[74] == 0); /* smile's top-row pixel */
	CHECK(led_frame[29] == 255 && led_frame[30] == 255); /* charger remains disabled */
	CHECK(available() == capacity);
	for (test_buttons = 0; test_buttons < 2; test_buttons++) {
		CHECK(native_fixture_run("matrix_fault") == 0);
		k_msleep(20);
		application_get_status(&status);
		printk("matrix fault: state %d result %d fault %u cleanup %d\n", status.state,
		       status.result, status.fault, status.cleanup_error);
		CHECK(status.state == APPLICATION_FAULT && status.cleanup_error == 0);
		CHECK(hub_matrix_get(&display) == 0 && display.owner == BP_MATRIX_FREE);
		CHECK(led_frame[77] == 0 && led_frame[78] == 0);
		CHECK(available() == capacity);
	}
	for (test_buttons = 0; test_buttons < 4; test_buttons++) {
		CHECK(native_fixture_run("light_fault") == 0);
		k_msleep(20);
		application_get_status(&status);
		CHECK(status.state == APPLICATION_FAULT && status.cleanup_error == 0);
		struct bp_light_state light;
		CHECK(hub_light_get(BP_LIGHT_CENTER, &light) == 0);
		CHECK(!light.owned && !light.pending && light.valid);
		CHECK(available() == capacity);
	}
	test_buttons = 5;
	CHECK(native_fixture_run("spin") == 0);
	k_msleep(20);
	CHECK(native_fixture_run("truncated") == -ENOEXEC);
	CHECK(native_fixture_run("read_error") == 0);
	CHECK(native_fixture_run("badabi") == -EPROTONOSUPPORT);
	CHECK(native_fixture_run("missingcap") == -ENOTSUP);
	CHECK(native_fixture_run("bigstack") == -ENOMEM);
	CHECK(native_fixture_run("badimport") == -ENOTSUP);
	CHECK(native_fixture_run("badreloc") == -ENOTSUP);
	CHECK(native_fixture_run("badfloat") == -ENOTSUP);
	CHECK(native_fixture_run("unwind") == -ENOTSUP);
	if (APPLICATION_MEMORY_LIMIT == 12288) {
		/* Tiny code sections separated across a 4 KiB image need a full
		 * MPU region, exceeding this loader's admission budget. */
		CHECK(native_fixture_run("sparse") == -ENOMEM);
	}
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RUNNING);
	k_sem_give(&stop_requested);
	CHECK(k_sem_take(&stop_completed, K_MSEC(100)) == 0 && stop_result == 0);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_STOPPED);
	CHECK(native_fixture_run("fault") == 0);
	k_msleep(20);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_FAULT);
	CHECK(native_fixture_run("hello") == 0);
	k_msleep(60);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RETURNED && status.result == 0);
	CHECK(available() == capacity);

	/* Staging is independent of the running application and connection owner. */
	CHECK(native_fixture_run("spin") == 0);
	size_t size, next;
	const uint8_t *image = native_fixture_hello(&size);
	CHECK(application_upload(APPLICATION_NATIVE, 0, size, image, 128, &next) == 0 && next == 128);
	CHECK(application_upload(APPLICATION_NATIVE, 129, size, image + 129, 1, &next) == -EINVAL);
	uint8_t changed = image[1] ^ 1;
	CHECK(application_upload(APPLICATION_NATIVE, 1, size, &changed, 1, &next) == -EINVAL);
	application_get_status(&status);
	CHECK(status.upload_bytes == 128 && status.state == APPLICATION_RUNNING);
	application_session_closed();
	application_poll();
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RUNNING && status.upload_size == 0);
	CHECK(application_run_uploaded(false) == -ENODATA);
	CHECK(application_upload(APPLICATION_NATIVE, 0, size - 1, image, size - 1, &next) == -ENOEXEC);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RUNNING && status.upload_size == 0);
	for (size_t offset = 0; offset < size;) {
		size_t count = MIN(128, size - offset);
		CHECK(application_upload(APPLICATION_NATIVE, offset, size, image + offset, count, &next) == 0);
		CHECK(next == offset + count);
		if (offset) {
			CHECK(application_upload(APPLICATION_NATIVE, offset, size, image + offset, count, &next) == 0);
		}
		offset = next;
	}
	application_session_closed();
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RUNNING && status.upload_ready);
	CHECK(application_run_uploaded(false) == 0);
	k_msleep(60);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_RETURNED && status.result == 0 && status.upload_size == 0);
	CHECK(available() == capacity);
	reservation = k_malloc(capacity);
	CHECK(reservation != NULL);
	CHECK(application_upload(APPLICATION_NATIVE, 0, size, image, 128, &next) == -ENOMEM);
	k_free(reservation);
	CHECK(available() == capacity);

	uint8_t timeout[4];
	sys_put_le32(1, timeout);
	CHECK(settings_runtime_set("power/battery_idle_s", timeout, 4) == 0);
	int64_t now = k_uptime_get();
	CHECK(!hub_power_update(0, true, now));
	CHECK(native_fixture_run("counter") == 0);
	CHECK(!hub_power_update(0, false, now + 2000));
	/* Reboot/DFU cleanup must reject new apps without sending the hub loop
	 * into power-off or revoking the connection executing the reset command. */
	CHECK(hub_power_connection(HUB_USB));
	CHECK(application_shutdown() == 0);
	application_get_status(&status);
	CHECK(status.state == APPLICATION_STOPPED && !status.cleanup_error);
	CHECK(!hub_power_update(0, false, now + 3000));
	CHECK(hub_power_connection(HUB_USB));
	CHECK(native_fixture_run("hello") == -ESHUTDOWN);
	CHECK(application_upload(APPLICATION_NATIVE, 0, size, image, 128, &next) == -ESHUTDOWN);
	CHECK(available() == capacity);
	CHECK(hub_power_connection(HUB_DISCONNECTED));
	CHECK(hub_power_update(0, false, k_uptime_get() + 2000));
	CHECK(!hub_power_application(true));
	printk("NATIVE PASS: lifecycle, MPU fault, forced stop, invalid image, power\n");
	return 0;
}
