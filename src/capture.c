// SPDX-License-Identifier: Apache-2.0
#include "capture.h"
#include "application.h"
#include "application_record.h"
#include "hub_clock.h"
#include "hub_imu.h"
#include "hub_battery.h"
#include "hub_ports.h"
#include "host_transport.h"
#include "motor_command.h"

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
enum { HEADER_SIZE = 28, CONFIG_SIZE = 16, STACK_SIZE = 2048,
	BENCH_CONFIG_SIZE = 48, STEP_SIZE = 24, MAX_STEPS = (1024 - BENCH_CONFIG_SIZE) / STEP_SIZE };
struct capture_step {
	uint32_t offset_ms, duration_ms, coast_mask;
	int16_t duty[BP_PORT_COUNT];
};

struct capture_plan {
	uint32_t mask, period_ms, duration_ms;
	struct bp_record_spec record;
	size_t record_memory, payload_bytes;
	uint32_t samples, snapshot_size, step_count, port_mask, generations[BP_PORT_COUNT];
	struct capture_step *steps;
};
struct capture_task {
	struct k_thread thread;
	k_thread_stack_t *stack;
	atomic_val_t epoch;
	uint32_t attempted, request, written;
	int64_t submitted_us;
	bool reported, settled, pending;
	uint8_t data[];
};
static K_MUTEX_DEFINE(mutex);
static K_SEM_DEFINE(stop, 0, 1);
static struct capture_plan plan;
static struct capture_task *task;
static bool owned, closing;
static int64_t origin_us;
static atomic_t missed, failure, cleanup_failure, epoch, scheduled_active;
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

/* A session boundary cancels only scheduled drive. Passive observations retain
 * their existing disconnect-independent lifetime. The epoch is never reset. */
void capture_session_closed(void)
{
	atomic_inc(&epoch);
	if (atomic_get(&scheduled_active)) k_sem_give(&stop);
}

static uint8_t *event(struct capture_task *run, uint32_t kind)
{
	sys_put_le32(kind, run->data);
	return run->data + 4;
}

static int group_event(struct capture_task *run, const struct bp_motor_group_state *state, int admission)
{
	uint8_t *out = event(run, 1);
	put32(&out, run->attempted);
	put64(&out, origin_us + (uint64_t)plan.steps[run->attempted - 1].offset_ms * 1000);
	put64(&out, run->submitted_us);
	put32(&out, run->request);
	put32(&out, admission);
	put32(&out, state->error);
	put32(&out, state->cleanup_error);
	put32(&out, (state->pending ? BIT(0) : 0) | (state->active ? BIT(1) : 0));
	put32(&out, state->applied_mask);
	put32(&out, state->cleanup_mask);
	put32(&out, state->timestamp_resolution_us ? state->timestamp_resolution_us : HUB_TIME_RESOLUTION_US);
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
		if (!(plan.port_mask & BIT(i))) continue;
		put32(&out, state->ports[i].generation);
		put32(&out, state->ports[i].request);
		put32(&out, state->ports[i].duty_permille);
		put32(&out, state->ports[i].mode);
		put64(&out, state->ports[i].applied_us);
		put64(&out, state->ports[i].cleanup_us);
		put32(&out, state->ports[i].error);
		put32(&out, state->ports[i].cleanup_error);
	}
	return application_record_write(run->data, out - run->data);
}

/* At most two results per action: initial application, then eventual cleanup.
 * Capture the old result before another action can replace the group's history. */
static int group_result(struct capture_task *run, const struct bp_motor_group_state *state)
{
	if (!run->request) return 0;
	if (state->request != run->request) return -ESTALE;
	run->pending = state->pending;
	if (state->pending) return 0;
	if (!run->reported || (!state->active && !run->settled)) {
		int err = group_event(run, state, 0);
		if (err) return err;
		run->reported = true;
		run->settled = !state->active;
	}
	return 0;
}

static int64_t action_end(const struct capture_task *run)
{
	const struct capture_step *step = &plan.steps[run->attempted - 1];
	return origin_us + ((uint64_t)step->offset_ms + step->duration_ms) * 1000;
}

static void run_schedule(void *context, void *unused1, void *unused2)
{
	struct capture_task *run = context;
	uint64_t period = (uint64_t)plan.period_ms * 1000;
	int64_t end = origin_us + (plan.duration_ms ? (uint64_t)plan.duration_ms * 1000 : plan.samples * period);
	uint32_t slot = 0, attempts = 0, reason = plan.duration_ms ? BP_RECORD_DURATION : BP_RECORD_FULL;
	int err = 0;
	struct bp_motor_group_state state = {0};
	for (;;) {
		if (atomic_get(&epoch) != run->epoch || !k_sem_take(&stop, K_NO_WAIT)) {
			reason = BP_RECORD_CANCELLED;
			break;
		}
		int64_t now = hub_time_us();
		if (run->request) {
			err = hub_motor_group_get(&state);
			if (!err) err = group_result(run, &state);
			if (!err && state.error && !(state.error == -ETIMEDOUT && now >= action_end(run))) err = state.error;
			if (!err) err = state.cleanup_error;
			if (err) break;
		}
		if (now >= end) {
			if (run->attempted < plan.step_count) err = -ETIMEDOUT;
			break;
		}
		if (run->attempted < plan.step_count &&
		    now >= origin_us + (uint64_t)plan.steps[run->attempted].offset_ms * 1000) {
			/* Missed actions cannot be replayed as a burst of delayed drive. */
			if (run->pending || (run->attempted + 1 < plan.step_count &&
			    now >= origin_us + (uint64_t)plan.steps[run->attempted + 1].offset_ms * 1000)) {
				err = -ETIMEDOUT;
				break;
			}
			uint32_t previous_request = run->request;
			int64_t previous_submission = run->submitted_us;
			bool previous_reported = run->reported, previous_settled = run->settled;
			const struct capture_step *step = &plan.steps[run->attempted++];
			attempts++;
			struct bp_motor_group_command command = {.port_mask = plan.port_mask,
				.coast_mask = step->coast_mask, .deadline_ms = action_end(run) / 1000};
			for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
				command.ports[i].generation = plan.generations[i];
				command.ports[i].duty_permille = step->duty[i];
			}
			run->request = 0;
			run->submitted_us = hub_time_us();
			run->reported = run->settled = false;
			err = hub_motor_group_guarded(BP_PORT_CAPTURE, &command, &run->request, &epoch, run->epoch);
			if (err) {
				state = (struct bp_motor_group_state){0};
				(void)group_event(run, &state, err);
				/* Rejection leaves the previous command in place. Its cleanup
				 * still belongs to the old action, not to this rejected one. */
				run->attempted--;
				run->request = previous_request;
				run->submitted_us = previous_submission;
				run->reported = previous_reported;
				run->settled = previous_settled;
				break;
			}
			run->pending = true;
			continue;
		}
		if (slot < plan.samples && now >= origin_us + slot * period) {
			uint32_t current = MIN((uint64_t)(now - origin_us) / period, plan.samples - 1);
			uint32_t skipped = current - slot;
			uint8_t *out = event(run, 0);
			put64(&out, origin_us + current * period);
			put64(&out, now);
			put64(&out, 0);
			put32(&out, skipped);
			observe(out);
			sys_put_le64(hub_time_us(), run->data + 20);
			err = application_record_write(run->data, 4 + plan.snapshot_size);
			if (err) break;
			run->written++;
			atomic_add(&missed, skipped);
			slot = current + 1;
			continue;
		}
		int64_t next = MIN(end, origin_us + slot * period);
		if (run->attempted < plan.step_count) {
			next = MIN(next, origin_us + (uint64_t)plan.steps[run->attempted].offset_ms * 1000);
		}
		if (run->request && !run->settled) {
			/* Pending driver completion is polled once per kernel tick. An
			 * already reported active command needs no polling until expiry. */
			int64_t poll = now + k_ticks_to_us_ceil64(1);
			next = MIN(next, run->pending || !run->reported ? poll : MAX(poll, action_end(run)));
		}
		if (!k_sem_take(&stop, K_USEC(MAX(0, next - hub_time_us())))) {
			reason = BP_RECORD_CANCELLED;
			break;
		}
	}
	/* Count only releases due before capture stopped, not the unused tail of
	 * a cancelled/failed experiment. Joining output cleanup can take longer. */
	int64_t stopped_us = hub_time_us();
	uint32_t due = stopped_us < origin_us ? 0 :
		MIN(plan.samples, 1 + (uint64_t)(stopped_us - origin_us) / period);
	if (due > slot) atomic_add(&missed, due - slot);
	/* Keep the reservation through copying cleanup history; another terminal
	 * command must not replace it while the final event is being assembled. */
	int cleanup = hub_motor_group_release(BP_PORT_CAPTURE, &state);
	int result = group_result(run, &state);
	if (!err) err = result;
	if (err == -ECANCELED && atomic_get(&epoch) != run->epoch) {
		reason = BP_RECORD_CANCELLED;
		err = 0;
	}
	if (err || cleanup) reason = BP_RECORD_ERROR;
	atomic_set(&failure, err);
	atomic_set(&cleanup_failure, cleanup);
	uint8_t *out = event(run, 3);
	put64(&out, hub_time_us());
	put32(&out, err);
	put32(&out, cleanup);
	put32(&out, atomic_get(&missed));
	put32(&out, plan.samples);
	put32(&out, run->written);
	put32(&out, attempts);
	result = application_record_write_final(run->data, out - run->data, reason);
	if (result) {
		if (!err) atomic_set(&failure, result);
		application_record_finish(BP_RECORD_ERROR);
	}
}

/* mutex is held. Joining precedes every stack, context and recorder release. */
static int join(bool wait)
{
	if (!task) return 0;
	if (wait) {
		atomic_inc(&epoch);
		k_sem_give(&stop);
	}
	int err = k_thread_join(&task->thread, wait ? K_FOREVER : K_NO_WAIT);
	if (err) return err;
	(void)k_thread_stack_space_get(&task->thread, &stack_unused);
	err = k_thread_stack_free(task->stack);
	if (err) return err;
	k_free(task);
	task = NULL;
	atomic_clear(&scheduled_active);
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
	if (plan.step_count) {
		shell_print(shell, "Variable records: %zu payload bytes; %u bytes maximum per record",
			plan.payload_bytes, plan.record.size);
	}
	shell_print(shell, "Samples: %u bytes; payload rate: %" PRIu64 " bytes/s; window: %" PRIu64 " ms",
		plan.snapshot_size + (plan.step_count ? 4 : 0),
		DIV_ROUND_UP((uint64_t)(plan.snapshot_size + (plan.step_count ? 6 : 0)) * 1000, plan.period_ms),
		plan.duration_ms ? (uint64_t)plan.duration_ms : (uint64_t)plan.samples * plan.period_ms);
	shell_print(shell, "Worker: %zu bytes + %zu stack bytes; allocator overhead separate",
		sizeof(struct capture_task) + MAX(plan.record.size, plan.step_count ? BENCH_CONFIG_SIZE + STEP_SIZE * plan.step_count : CONFIG_SIZE), (size_t)K_THREAD_STACK_LEN(STACK_SIZE));
	struct application_memory memory;
	if (!application_memory_get(&memory)) {
		shell_print(shell, "Shared heap free: %zu bytes; start still requires allocation", memory.heap_free);
	}
	for (unsigned int i = 0; i < SOURCE_COUNT; i++) {
		if (plan.mask & BIT(i)) shell_print(shell, "  %s", sources[i]);
	}
	if (plan.step_count) {
		shell_print(shell, "Schedule: %u actions; %u sample slots; %u event slots; %zu plan bytes",
			plan.step_count, plan.samples, 2 * plan.step_count + 2, plan.step_count * sizeof(*plan.steps));
		for (uint32_t i = 0; i < plan.step_count; i++) {
			const struct capture_step *step = &plan.steps[i];
			shell_fprintf(shell, SHELL_NORMAL, "  at %u %u", step->offset_ms, step->duration_ms);
			for (uint32_t port = 0; port < BP_PORT_COUNT; port++) {
				if (!(plan.port_mask & BIT(port))) continue;
				if (step->coast_mask & BIT(port)) shell_fprintf(shell, SHELL_NORMAL, " %c=coast", 'A' + port);
				else shell_fprintf(shell, SHELL_NORMAL, " %c=%d", 'A' + port, step->duty[port]);
			}
			shell_print(shell, "");
		}
	}
	if (owned) {
		shell_print(shell, "Cleanup error: %d", (int)atomic_get(&cleanup_failure));
		shell_print(shell, "Records: %" PRIu64 "; origin: %" PRId64 " us; missed: %u; reason: %u; error: %d",
			info.next, origin_us, (uint32_t)atomic_get(&missed), info.reason, (int)atomic_get(&failure));
		if (!task) shell_print(shell, "Worker stack unused: %zu bytes", stack_unused);
	}
}

static int budget(const struct shell *shell, struct capture_plan *candidate)
{
	uint32_t count = candidate->step_count;
	size_t config_size = count ? BENCH_CONFIG_SIZE + count * STEP_SIZE : CONFIG_SIZE;
	size_t sample_bytes = candidate->snapshot_size + (count ? 4 : 0);
	size_t group_bytes = 56 + 40 * POPCOUNT(candidate->port_mask);
	candidate->record.size = count ? MAX(sample_bytes, group_bytes) : sample_bytes;
	strcpy(candidate->record.schema, count ? "brickphyr.bench/2" : "brickphyr.capture/1");
	uint32_t events = count ? 2 * count + 2 : 0;
	size_t event_bytes = count ? 2 * count * group_bytes + 12 + 36 : 0;
	uint32_t capacity = (32768 - config_size) / sample_bytes;
	if (count) {
		/* Include event payloads, offset index and block pointers in the
		 * actual RAM limit. Find the largest admitted count using the same
		 * layout validation as allocation, without allocating a probe. */
		uint32_t low = 0, high = capacity;
		while (low < high) {
			uint32_t middle = low + (high - low + 1) / 2;
			size_t bytes;
			int err = application_record_compact_budget(candidate->record.size, middle + events,
				config_size, middle * sample_bytes + event_bytes, &bytes);
			if (err) high = middle - 1;
			else low = middle;
		}
		capacity = low;
	}
	uint64_t samples = candidate->duration_ms ?
		DIV_ROUND_UP((uint64_t)candidate->duration_ms, candidate->period_ms) :
		capacity;
	if (!samples || samples > capacity) {
		shell_error(shell, "Capture needs %" PRIu64 " samples; capacity is %u after %u event reservations",
			samples, capacity, events);
		return -ENOSPC;
	}
	uint64_t duration = candidate->duration_ms ? candidate->duration_ms : samples * candidate->period_ms;
	for (uint32_t i = 0; i < count; i++) {
		const struct capture_step *step = &candidate->steps[i];
		if (step->offset_ms >= duration || (uint64_t)step->offset_ms + step->duration_ms > duration) {
			shell_error(shell, "Action %u exceeds the %" PRIu64 " ms capture window", i + 1, duration);
			return -ERANGE;
		}
	}
	candidate->samples = samples;
	candidate->record.capacity = samples + events;
	candidate->payload_bytes = count ? samples * sample_bytes + event_bytes : 0;
	if (count) return application_record_compact_budget(candidate->record.size, candidate->record.capacity,
		config_size, candidate->payload_bytes, &candidate->record_memory);
	return application_record_budget(candidate->record.size, candidate->record.capacity,
		config_size, false, 0, &candidate->record_memory);
}

static int reserve(const struct capture_plan *candidate, bool claim)
{
	if (!candidate->step_count) return 0;
	struct bp_motor_group_command command = {.port_mask = candidate->port_mask,
		.coast_mask = candidate->port_mask};
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) command.ports[i].generation = candidate->generations[i];
	return hub_motor_group_prepare(BP_PORT_CAPTURE, &command, claim);
}

static int append(const struct shell *shell, size_t argc, char **argv)
{
	if (owned) return -EBUSY;
	if (!plan.mask) return -ENODATA;
	if (plan.step_count == MAX_STEPS) return -ENOSPC;
	int err = 0;
	unsigned long offset = shell_strtoul(argv[1], 10, &err);
	if (err || argv[1][0] == '-' || offset > UINT32_MAX ||
	    (plan.step_count && offset <= plan.steps[plan.step_count - 1].offset_ms)) return -EINVAL;
	struct bp_motor_group_command command;
	uint32_t duration;
	err = motor_command_parse(argc - 2, argv + 2, &command, &duration);
	if (err) return err;
	if (plan.step_count && command.port_mask != plan.port_mask) return -EINVAL;
	struct capture_plan candidate = plan;
	candidate.port_mask = command.port_mask;
	if (!plan.step_count) {
		for (uint32_t i = 0; i < BP_PORT_COUNT; i++) {
			if (!(command.port_mask & BIT(i))) continue;
			struct bp_port_state state;
			err = hub_port_get(i, &state);
			if (err) return err;
			candidate.generations[i] = state.generation;
		}
	}
	candidate.step_count++;
	err = reserve(&candidate, false);
	if (err) return err;
	candidate.steps = k_malloc(candidate.step_count * sizeof(*candidate.steps));
	if (!candidate.steps) return -ENOMEM;
	if (plan.step_count) memcpy(candidate.steps, plan.steps, plan.step_count * sizeof(*plan.steps));
	struct capture_step *step = &candidate.steps[plan.step_count];
	*step = (struct capture_step){.offset_ms = offset, .duration_ms = duration, .coast_mask = command.coast_mask};
	for (uint32_t i = 0; i < BP_PORT_COUNT; i++) step->duty[i] = command.ports[i].duty_permille;
	err = budget(shell, &candidate);
	if (err) k_free(candidate.steps);
	else {
		k_free(plan.steps);
		plan = candidate;
		shell_print(shell, "Action %u prepared; no outputs changed", plan.step_count);
	}
	return err;
}

static int configure(const struct shell *shell, size_t argc, char **argv)
{
	if (owned) return -EBUSY;
	int err = 0;
	unsigned long period = shell_strtoul(argv[1], 10, &err);
	unsigned long duration = !strcmp(argv[2], "full") ? 0 : shell_strtoul(argv[2], 10, &err);
	if (err || argv[1][0] == '-' || argv[2][0] == '-' || !period || period > UINT32_MAX || duration > UINT32_MAX ||
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
	candidate.snapshot_size = candidate.record.size;
	err = budget(shell, &candidate);
	if (err) return err;
	k_free(plan.steps);
	plan = candidate;
	show(shell);
	return 0;
}

static int start(void)
{
	if (closing) return -ESHUTDOWN;
	if (!plan.mask) return -ENODATA;
	if (owned || task) return -EBUSY;
	atomic_val_t expected = atomic_get(&epoch);
	if (plan.step_count && !host_session_open()) return -ENOTCONN;
	int err = reserve(&plan, false);
	if (err) return err;
	size_t config_size = plan.step_count ? BENCH_CONFIG_SIZE + STEP_SIZE * plan.step_count : CONFIG_SIZE;
	struct capture_task *run = k_calloc(1, sizeof(*run) + MAX(plan.record.size, config_size));
	if (!run) return -ENOMEM;
	run->epoch = expected;
	run->stack = k_thread_stack_alloc(STACK_SIZE, 0);
	if (!run->stack) {
		k_free(run);
		return -ENOMEM;
	}
	uint8_t *out = run->data;
	put32(&out, plan.mask);
	put32(&out, plan.period_ms);
	put32(&out, plan.duration_ms);
	put32(&out, HUB_TIME_RESOLUTION_US);
	if (plan.step_count) {
		put32(&out, plan.port_mask);
		put32(&out, plan.step_count);
		for (uint32_t i = 0; i < BP_PORT_COUNT; i++) put32(&out, plan.generations[i]);
		for (uint32_t i = 0; i < plan.step_count; i++) {
			const struct capture_step *step = &plan.steps[i];
			put32(&out, step->offset_ms);
			put32(&out, step->duration_ms);
			put32(&out, step->coast_mask);
			for (uint32_t port = 0; port < BP_PORT_COUNT; port++, out += 2) sys_put_le16(step->duty[port], out);
		}
	}
	err = application_capture_begin(&plan.record, run->data, config_size,
		plan.duration_ms ? BP_RECORD_DURATION : BP_RECORD_FULL, plan.payload_bytes);
	if (!err) {
		err = reserve(&plan, true);
		if (err) {
			application_record_finish(BP_RECORD_CANCELLED);
			(void)application_capture_release();
		}
	}
	if (err) {
		(void)k_thread_stack_free(run->stack);
		k_free(run);
		return err;
	}
	owned = true;
	task = run;
	k_sem_reset(&stop);
	atomic_clear(&failure);
	atomic_clear(&cleanup_failure);
	atomic_clear(&missed);
	stack_unused = 0;
	origin_us = hub_time_us();
	if (plan.step_count) {
		/* Actions and their driver deadlines are expressed in milliseconds.
		 * Use the next common boundary, never truncate a requested duration. */
		origin_us = DIV_ROUND_UP(origin_us, 1000) * 1000;
		atomic_set(&scheduled_active, 1);
		out = event(run, 2);
		put64(&out, origin_us);
		(void)application_record_write(run->data, out - run->data);
	}
	/* Below hardware/management, above the primary app, like native control.
	 * Sampling copies snapshots; only the shared port worker applies outputs. */
	k_thread_create(&run->thread, run->stack, STACK_SIZE, plan.step_count ? run_schedule : sample,
		run, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO - 2, 0, K_FOREVER);
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
	else if (!strcmp(argv[0], "at")) err = append(shell, argc, argv);
	else if (!strcmp(argv[0], "check")) {
		err = owned ? -EBUSY : !plan.mask ? -ENODATA : reserve(&plan, false);
		if (!err) show(shell);
	} else if (!strcmp(argv[0], "start")) err = start();
	else if (!strcmp(argv[0], "stop")) {
		err = join(true);
		if (!err) err = atomic_get(&failure);
		if (!err) err = atomic_get(&cleanup_failure);
	}
	else if (!strcmp(argv[0], "clear")) {
		struct bp_record_info info;
		application_record_info(&info);
		err = owned && info.active ? -EBUSY : join(false);
		if (!err && owned && plan.step_count) err = hub_motor_group_release(BP_PORT_CAPTURE, NULL);
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
	SHELL_CMD_ARG(at, NULL, "offset_ms duration_ms A=duty|brake|coast ...", command, 4, BP_PORT_COUNT - 1),
	SHELL_CMD_ARG(check, NULL, "Validate prepared ports and show the plan without actuation.", command, 1, 0),
	SHELL_CMD_ARG(start, NULL, "Allocate and start the prepared capture/schedule.", command, 1, 0),
	SHELL_CMD_ARG(status, NULL, "Show plan, memory and retained capture.", command, 1, 0),
	SHELL_CMD_ARG(stop, NULL, "Stop sampling and scheduled outputs; retain records.", command, 1, 0),
	SHELL_CMD_ARG(clear, NULL, "Discard stopped capture and release its power hold.", command, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(capture, &commands, "Finite RAM observations and explicitly started motor schedules.", NULL);
