// SPDX-License-Identifier: Apache-2.0
#include "capture.h"
#include "application.h"
#include "application_record.h"
#include "hub_clock.h"
#include "hub_imu.h"
#include "hub_battery.h"
#include "hub_ports.h"

#include <inttypes.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

/* brickphyr.capture/1: explicit little-endian fields, no C padding or field
 * names per sample. Configuration is mask, period_ms, duration_ms, clock_us.
 * Each sample begins with scheduled/start/end us and missed releases (28 B).
 * Blocks follow mask order; source times remain acquisition/service times. */
enum { RAW_IMU, CALIBRATED_IMU, BATTERY, PORT_SOURCE, SOURCE_COUNT = 15 };
static const char *const sources[] = {"imu", "imu.calibrated", "battery",
	"encoder.A", "motor.A", "encoder.B", "motor.B", "encoder.C", "motor.C",
	"encoder.D", "motor.D", "encoder.E", "motor.E", "encoder.F", "motor.F"};
static const uint8_t sizes[] = {56, 68, 44, 28, 52, 28, 52, 28, 52, 28, 52, 28, 52, 28, 52};
enum { HEADER_SIZE = 28, CONFIG_SIZE = 16, STACK_SIZE = 2048 };

struct capture_plan {
	uint32_t mask, period_ms, duration_ms;
	struct bp_record_spec record;
	size_t record_memory;
};
struct capture_task {
	struct k_thread thread;
	k_thread_stack_t *stack;
	uint8_t data[];
};
static K_MUTEX_DEFINE(mutex);
static K_SEM_DEFINE(stop, 0, 1);
static struct capture_plan plan;
static struct capture_task *task;
static bool owned, closing;
static int64_t origin_us;
static atomic_t missed, failure;
static size_t stack_unused;

static bool available(unsigned int source)
{
	if (source <= CALIBRATED_IMU) return IS_ENABLED(CONFIG_BRICKPHYR_IMU);
	if (source == BATTERY) return IS_ENABLED(CONFIG_BRICKPHYR_BATTERY);
	bool enabled = (source - PORT_SOURCE) & 1 ? IS_ENABLED(CONFIG_BRICKPHYR_MOTORS) :
		IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART);
	struct bp_control_port value;
	return enabled && !hub_control_port_get((source - PORT_SOURCE) / 2, &value);
}

static void put32(uint8_t **out, uint32_t value)
{
	sys_put_le32(value, *out);
	*out += 4;
}

static void put64(uint8_t **out, int64_t value)
{
	sys_put_le64(value, *out);
	*out += 8;
}

static void imu_bytes(uint8_t **out, const struct bp_imu_sample *raw,
	const struct bp_imu_state *calibrated)
{
	put64(out, raw->sample_us);
	put64(out, raw->read_us);
	put32(out, raw->sequence);
	put32(out, raw->missed);
	put32(out, raw->failures);
	put32(out, raw->error);
	const int32_t *acceleration = calibrated ? calibrated->acceleration : raw->acceleration;
	const int32_t *angular_velocity = calibrated ? calibrated->angular_velocity : raw->angular_velocity;
	for (size_t i = 0; i < 3; i++) put32(out, acceleration[i]);
	for (size_t i = 0; i < 3; i++) put32(out, angular_velocity[i]);
	if (calibrated) {
		put32(out, calibrated->epoch);
		put32(out, calibrated->error);
		put32(out, (calibrated->ready ? BIT(0) : 0) | (calibrated->stationary ? BIT(1) : 0));
	}
}

static void observe(uint8_t *out)
{
	/* Selected raw/calibrated IMU and each encoder/output pair share one service
	 * snapshot. This does not claim that different devices latch simultaneously. */
	if (plan.mask & (BIT(RAW_IMU) | BIT(CALIBRATED_IMU))) {
		struct bp_imu_state value = {0};
		int err = plan.mask & BIT(CALIBRATED_IMU) ? hub_imu_get_state(&value) : hub_imu_get(&value.raw);
		if (err) value.error = value.raw.error = err;
		if (plan.mask & BIT(RAW_IMU)) imu_bytes(&out, &value.raw, NULL);
		if (plan.mask & BIT(CALIBRATED_IMU)) imu_bytes(&out, &value.raw, &value);
	}
	if (plan.mask & BIT(BATTERY)) {
		struct bp_battery_sample value = {0};
		int err = hub_battery_get(&value);
		if (err) value.error = err;
		put64(&out, value.sample_ms);
		put64(&out, value.read_ms);
		put32(&out, value.voltage_mv);
		put32(&out, value.current_ma);
		put32(&out, value.sequence);
		put32(&out, value.failures);
		put32(&out, value.error);
		put32(&out, value.average_mv);
		put32(&out, (value.low ? BIT(0) : 0) | (value.critical ? BIT(1) : 0));
	}
	for (unsigned int port = 0; port < BP_PORT_COUNT; port++) {
		unsigned int encoder = PORT_SOURCE + port * 2;
		if (!(plan.mask & (3U << encoder))) continue;
		struct bp_control_port value = {0};
		int err = hub_control_port_get(port, &value);
		if (err) value.error = value.output_error = err;
		if (plan.mask & BIT(encoder)) {
			put64(&out, value.sample_ms);
			put64(&out, value.angle_mdeg);
			put32(&out, value.generation);
			put32(&out, value.sequence);
			put32(&out, value.error);
		}
		if (plan.mask & BIT(encoder + 1)) {
			put64(&out, value.requested_ms);
			put64(&out, value.applied_ms);
			put32(&out, value.generation);
			put32(&out, value.request);
			put32(&out, value.completed);
			put32(&out, value.request_generation);
			put32(&out, value.output_generation);
			put32(&out, value.output_error);
			put32(&out, value.cleanup_error);
			put32(&out, value.duty_permille);
			*out++ = value.mode;
			*out++ = value.valid;
			*out++ = value.pending;
			*out++ = value.owner;
		}
	}
}

static void sample(void *context, void *unused1, void *unused2)
{
	struct capture_task *run = context;
	uint64_t period = (uint64_t)plan.period_ms * 1000;
	uint64_t duration = (uint64_t)plan.duration_ms * 1000;
	uint64_t slot = 0;
	for (;;) {
		int64_t now = hub_time_us();
		int64_t scheduled = origin_us + slot * period;
		if (!k_sem_take(&stop, now < scheduled ? K_USEC(scheduled - now) : K_NO_WAIT)) {
			application_record_finish(BP_RECORD_CANCELLED);
			return;
		}
		now = hub_time_us();
		if (now < scheduled) continue;
		uint64_t current = (now - origin_us) / period;
		uint64_t skipped = current - slot;
		if (skipped > UINT32_MAX - (uint32_t)atomic_get(&missed)) {
			atomic_set(&failure, -EOVERFLOW);
			application_record_finish(BP_RECORD_ERROR);
			return;
		}
		if (duration && (uint64_t)(now - origin_us) >= duration) {
			/* Includes releases missed at the end, with no later sample in
			 * which to carry them. Capacity - final count exposes them in the log. */
			atomic_add(&missed, plan.record.capacity - slot);
			application_record_finish(BP_RECORD_DURATION);
			return;
		}
		slot = current;
		uint8_t *out = run->data;
		put64(&out, origin_us + slot * period);
		put64(&out, now);
		put64(&out, 0); /* Completion is published only with the whole sample. */
		put32(&out, skipped);
		observe(out);
		sys_put_le64(hub_time_us(), run->data + 16);
		int err = application_record_write(run->data, plan.record.size);
		if (err) {
			/* A host trigger may have frozen it while this snapshot was copied. */
			if (err != -ENOSPC) {
				atomic_set(&failure, err);
				application_record_finish(BP_RECORD_ERROR);
			}
			return;
		}
		atomic_add(&missed, skipped);
		struct bp_record_info info;
		application_record_info(&info);
		if (!info.active) return;
		slot++;
	}
}

/* mutex is held. Joining precedes every stack, context and recorder release. */
static int join(bool wait)
{
	if (!task) return 0;
	if (wait) k_sem_give(&stop);
	int err = k_thread_join(&task->thread, wait ? K_FOREVER : K_NO_WAIT);
	if (err) return err;
	(void)k_thread_stack_space_get(&task->thread, &stack_unused);
	err = k_thread_stack_free(task->stack);
	if (err) return err;
	k_free(task);
	task = NULL;
	return 0;
}

void capture_poll(void)
{
	if (k_mutex_lock(&mutex, K_NO_WAIT)) return;
	(void)join(false);
	k_mutex_unlock(&mutex);
}

int capture_shutdown(void)
{
	k_mutex_lock(&mutex, K_FOREVER);
	closing = true;
	int err = join(true);
	k_mutex_unlock(&mutex);
	return err;
}

static void show(const struct shell *shell)
{
	struct bp_record_info info;
	application_record_info(&info);
	shell_print(shell, "Capture: %s", owned ? info.active ? "running" : "retained" :
		plan.mask ? "prepared" : "unconfigured");
	if (!plan.mask) return;
	shell_print(shell, "Period: %u ms; duration: %u ms (0: until full)", plan.period_ms, plan.duration_ms);
	shell_print(shell, "Record: %u bytes; capacity: %u; buffer: %zu bytes",
		plan.record.size, plan.record.capacity, plan.record_memory);
	shell_print(shell, "Worker: %zu bytes + %zu stack bytes; allocator overhead separate",
		sizeof(struct capture_task) + plan.record.size, (size_t)K_THREAD_STACK_LEN(STACK_SIZE));
	struct application_memory memory;
	if (!application_memory_get(&memory)) {
		shell_print(shell, "Shared heap free: %zu bytes; start still requires allocation", memory.heap_free);
	}
	for (unsigned int i = 0; i < SOURCE_COUNT; i++) {
		if (plan.mask & BIT(i)) shell_print(shell, "  %s", sources[i]);
	}
	if (owned) {
		shell_print(shell, "Records: %" PRIu64 "; origin: %" PRId64 " us; missed: %u; reason: %u; error: %d",
			info.next, origin_us, (uint32_t)atomic_get(&missed), info.reason, (int)atomic_get(&failure));
		if (!task) shell_print(shell, "Worker stack unused: %zu bytes", stack_unused);
	}
}

static int configure(const struct shell *shell, size_t argc, char **argv)
{
	if (owned) return -EBUSY;
	int err = 0;
	unsigned long period = shell_strtoul(argv[1], 10, &err);
	unsigned long duration = !strcmp(argv[2], "full") ? 0 : shell_strtoul(argv[2], 10, &err);
	if (err || !period || period > UINT32_MAX || duration > UINT32_MAX ||
	    (!duration && strcmp(argv[2], "full"))) return -EINVAL;
	struct capture_plan candidate = {.period_ms = period, .duration_ms = duration,
		.record = {.size = HEADER_SIZE, .schema = "brickphyr.capture/1"}};
	for (size_t arg = 3; arg < argc; arg++) {
		if (!strcmp(argv[arg], "all")) {
			if (argc != 4) return -EINVAL;
			for (unsigned int i = 0; i < SOURCE_COUNT; i++) {
				if (!available(i)) continue;
				candidate.mask |= BIT(i);
				candidate.record.size += sizes[i];
			}
			break;
		}
		unsigned int i;
		for (i = 0; i < SOURCE_COUNT; i++) if (!strcmp(argv[arg], sources[i])) break;
		if (i == SOURCE_COUNT) return -ENOENT;
		if (!available(i)) return -ENOTSUP;
		if (candidate.mask & BIT(i)) return -EINVAL;
		candidate.mask |= BIT(i);
		candidate.record.size += sizes[i];
	}
	if (!candidate.mask) return -ENOTSUP;
	uint32_t capacity = (32768 - CONFIG_SIZE) / candidate.record.size;
	uint64_t count = duration ? DIV_ROUND_UP((uint64_t)duration, period) : capacity;
	if (count > capacity) {
		shell_error(shell, "Capture needs %" PRIu64 " records; capacity is %u (%" PRIu64 " ms)",
			count, capacity, (uint64_t)capacity * period);
		return -ENOSPC;
	}
	candidate.record.capacity = count;
	err = application_record_budget(candidate.record.size, count, CONFIG_SIZE, false, 0,
		&candidate.record_memory);
	if (err) return err;
	plan = candidate;
	show(shell);
	return 0;
}

static int start(void)
{
	if (closing) return -ESHUTDOWN;
	if (!plan.mask) return -ENODATA;
	if (owned || task) return -EBUSY;
	struct capture_task *run = k_malloc(sizeof(*run) + plan.record.size);
	if (!run) return -ENOMEM;
	run->stack = k_thread_stack_alloc(STACK_SIZE, 0);
	if (!run->stack) {
		k_free(run);
		return -ENOMEM;
	}
	uint8_t config[CONFIG_SIZE];
	sys_put_le32(plan.mask, config);
	sys_put_le32(plan.period_ms, config + 4);
	sys_put_le32(plan.duration_ms, config + 8);
	sys_put_le32(HUB_TIME_RESOLUTION_US, config + 12);
	int err = application_capture_begin(&plan.record, config, sizeof(config),
		plan.duration_ms ? BP_RECORD_DURATION : BP_RECORD_FULL);
	if (err) {
		(void)k_thread_stack_free(run->stack);
		k_free(run);
		return err;
	}
	owned = true;
	task = run;
	k_sem_reset(&stop);
	atomic_clear(&failure);
	atomic_clear(&missed);
	stack_unused = 0;
	origin_us = hub_time_us();
	/* Below hardware/management, above the primary app, like native control.
	 * Sampling only copies bounded service snapshots; it never performs I/O. */
	k_thread_create(&run->thread, run->stack, STACK_SIZE, sample, run, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO - 2, 0, K_FOREVER);
	k_thread_name_set(&run->thread, "capture");
	k_thread_start(&run->thread);
	return 0;
}

static int command(const struct shell *shell, size_t argc, char **argv)
{
	k_mutex_lock(&mutex, K_FOREVER);
	(void)join(false);
	int err;
	if (!strcmp(argv[0], "sources")) {
		for (unsigned int i = 0; i < SOURCE_COUNT; i++) {
			if (available(i)) shell_print(shell, "%s: %u bytes", sources[i], sizes[i]);
		}
		err = 0;
	} else if (!strcmp(argv[0], "prepare")) err = configure(shell, argc, argv);
	else if (!strcmp(argv[0], "start")) err = start();
	else if (!strcmp(argv[0], "stop")) err = join(true);
	else if (!strcmp(argv[0], "clear")) {
		struct bp_record_info info;
		application_record_info(&info);
		err = owned && info.active ? -EBUSY : join(false);
		if (!err) err = application_capture_release();
		if (!err) owned = false;
	} else {
		show(shell);
		err = 0;
	}
	if (err) shell_error(shell, "Capture failed: %d", err);
	k_mutex_unlock(&mutex);
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(commands,
	SHELL_CMD_ARG(sources, NULL, "List available passive sources and their record sizes.", command, 1, 0),
	SHELL_CMD_ARG(prepare, NULL, "period_ms duration_ms|full all|source ...", command, 4, SOURCE_COUNT - 1),
	SHELL_CMD_ARG(start, NULL, "Allocate and start the prepared passive capture.", command, 1, 0),
	SHELL_CMD_ARG(status, NULL, "Show plan, memory and retained capture.", command, 1, 0),
	SHELL_CMD_ARG(stop, NULL, "Stop sampling; retain records.", command, 1, 0),
	SHELL_CMD_ARG(clear, NULL, "Discard stopped capture and release its power hold.", command, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(capture, &commands, "Finite RAM observations; no device writes.", NULL);
