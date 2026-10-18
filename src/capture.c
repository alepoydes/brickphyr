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
#include "telemetry_block.h"

#include <inttypes.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#define CAPTURE_FIELDS_DESCRIPTORS
#include <brickphyr_capture.h>
BUILD_ASSERT(CONFIG_SIZE == BP_CAPTURE_CONFIG_SIZE);

/* Capture rows use the capture/2 field layout. Capture/3 and bench/4 retain
 * these exact values in independently decodable BCB3 blocks. Source times
 * remain acquisition/service times, not inferred from the capture clock. */
enum { RAW_IMU, CALIBRATED_IMU, BATTERY, PORT_SOURCE };
enum { HEADER_SIZE = 28, STACK_SIZE = 2048,
	BENCH_CONFIG_SIZE = CONFIG_SIZE + 32, STEP_SIZE = 24,
	MAX_STEPS = (1024 - BENCH_CONFIG_SIZE) / STEP_SIZE };
struct capture_step {
	uint32_t offset_ms, duration_ms, coast_mask;
	int16_t duty[BP_PORT_COUNT];
};

struct capture_plan {
	uint32_t mask, period_ms, duration_ms;
	uint8_t fields[SOURCE_COUNT];
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
	uint16_t field_count;
	struct telemetry_block block;
	struct bp_control_frame frame;
	struct telemetry_wire_field wire[CAPTURE_MAX_FIELDS];
	uint8_t block_data[1024];
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

static uint32_t source_size(unsigned int source, uint8_t selected)
{
	const struct capture_fields *fields = &field_kinds[source_kinds[source]];
	uint32_t size = fields->base;
	for (unsigned int i = 0; i < fields->count; i++) {
		if (selected & BIT(i)) size += fields->items[i].size;
	}
	return size;
}

static void show_source(const struct shell *shell, unsigned int source, uint8_t selected)
{
	const struct capture_fields *fields = &field_kinds[source_kinds[source]];
	shell_fprintf(shell, SHELL_NORMAL, "  %s:", sources[source]);
	bool first = true;
	for (unsigned int i = 0; i < fields->count; i++) {
		if (!(selected & BIT(i))) continue;
		shell_fprintf(shell, SHELL_NORMAL, "%s%s", first ? "" : ",", fields->items[i].name);
		first = false;
	}
	shell_print(shell, " (%u bytes, including %u required)", source_size(source, selected), fields->base);
}

static int select_source(struct capture_plan *candidate, const char *argument)
{
	size_t length = strcspn(argument, ":");
	unsigned int source;
	for (source = 0; source < SOURCE_COUNT; source++) {
		if (strlen(sources[source]) == length && !strncmp(argument, sources[source], length)) break;
	}
	if (source == SOURCE_COUNT) return -ENOENT;
	if (!available(source)) return -ENOTSUP;
	if (candidate->mask & BIT(source)) return -EINVAL;
	const struct capture_fields *fields = &field_kinds[source_kinds[source]];
	uint8_t selected = BIT(fields->count) - 1;
	if (argument[length]) {
		selected = 0;
		const char *next = argument + length + 1;
		for (;;) {
			length = strcspn(next, ",");
			if (!length) return -EINVAL;
			unsigned int field;
			for (field = 0; field < fields->count; field++) {
				if (strlen(fields->items[field].name) == length &&
				    !strncmp(next, fields->items[field].name, length)) break;
			}
			if (field == fields->count) return -ENOENT;
			if (selected & BIT(field)) return -EINVAL;
			selected |= BIT(field);
			next += length;
			if (!*next) break;
			next++;
		}
	}
	candidate->mask |= BIT(source);
	candidate->fields[source] = selected;
	candidate->record.size += source_size(source, selected);
	return 0;
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

static uint8_t *pack_sources(uint32_t mask, const uint8_t fields[SOURCE_COUNT],
	const struct bp_control_frame *frame, uint8_t *out)
{
	if (mask & BIT(RAW_IMU)) out = capture_pack_imu(out, fields[RAW_IMU], &frame->imu.raw);
	if (mask & BIT(CALIBRATED_IMU)) {
		out = capture_pack_imu_calibrated(out, fields[CALIBRATED_IMU], &frame->imu);
	}
	if (mask & BIT(BATTERY)) out = capture_pack_battery(out, fields[BATTERY], &frame->battery);
	for (unsigned int port = 0; port < BP_PORT_COUNT; port++) {
		unsigned int encoder = PORT_SOURCE + port * 2;
		if (mask & BIT(encoder)) out = capture_pack_encoder(out, fields[encoder], &frame->ports[port]);
		if (mask & BIT(encoder + 1)) {
			out = capture_pack_motor(out, fields[encoder + 1], &frame->ports[port]);
		}
	}
	return out;
}

int capture_pack_frame(const uint8_t config[CONFIG_SIZE], const struct bp_control_step *step,
	const struct bp_control_frame *frame, void *data, size_t capacity)
{
	uint32_t mask = sys_get_le32(config);
	uint32_t resolution = sys_get_le32(config + 12);
	if (!mask || mask >> SOURCE_COUNT || !sys_get_le32(config + 4) ||
	    !resolution || resolution != frame->capture_resolution_us ||
	    !step->sequence || frame->port_mask >> BP_PORT_COUNT ||
	    frame->capture_start_us > frame->capture_end_us) return -EINVAL;
	size_t length = HEADER_SIZE;
	for (unsigned int source = 0; source < SOURCE_COUNT; source++) {
		uint8_t selected = config[16 + source];
		if (!(mask & BIT(source))) {
			if (selected) return -EINVAL;
			continue;
		}
		const struct capture_fields *kind = &field_kinds[source_kinds[source]];
		if (!selected || selected & ~((1U << kind->count) - 1)) return -EINVAL;
		if (source >= PORT_SOURCE && !(frame->port_mask & BIT((source - PORT_SOURCE) / 2))) {
			return -EINVAL;
		}
		length += source_size(source, selected);
	}
	if (length > capacity) return -EMSGSIZE;
	uint8_t *out = data;
	put64(&out, step->scheduled_us);
	put64(&out, frame->capture_start_us);
	put64(&out, frame->capture_end_us);
	put32(&out, step->skipped);
	out = pack_sources(mask, config + 16, frame, out);
	return out - (uint8_t *)data;
}

static void observe(struct capture_task *run, uint8_t *out)
{
	/* Selected raw/calibrated IMU and each encoder/output pair share one service
	 * snapshot. This does not claim that different devices latch simultaneously. */
	struct bp_control_frame *frame = &run->frame;
	memset(frame, 0, sizeof(*frame));
	if (plan.mask & (BIT(RAW_IMU) | BIT(CALIBRATED_IMU))) {
		int err = plan.mask & BIT(CALIBRATED_IMU) ?
			hub_imu_get_state(&frame->imu) : hub_imu_get(&frame->imu.raw);
		if (err) frame->imu.error = frame->imu.raw.error = err;
	}
	if (plan.mask & BIT(BATTERY)) {
		int err = hub_battery_get(&frame->battery);
		if (err) frame->battery.error = err;
	}
	for (unsigned int port = 0; port < BP_PORT_COUNT; port++) {
		unsigned int encoder = PORT_SOURCE + port * 2;
		if (!(plan.mask & (3U << encoder))) continue;
		int err = hub_control_port_get(port, &frame->ports[port]);
		if (err) frame->ports[port].error = frame->ports[port].output_error = err;
	}
	(void)pack_sources(plan.mask, plan.fields, frame, out);
}

static size_t wire_layout(uint32_t mask, const uint8_t fields[SOURCE_COUNT], size_t row_size,
	struct telemetry_wire_field *wire, size_t capacity)
{
	static const struct { uint8_t size, delta; } header[] = {{8, 1}, {8, 1}, {8, 1}, {4, 1}};
	size_t count = 0, offset = 0;
	for (size_t i = 0; i < ARRAY_SIZE(header); i++) {
		if (wire && count == capacity) return 0;
		if (wire) wire[count] = (struct telemetry_wire_field){offset, header[i].size, -1, header[i].delta};
		count++;
		offset += header[i].size;
	}
	int raw_base = -1;
	for (unsigned int source = 0; source < SOURCE_COUNT; source++) {
		if (!(mask & BIT(source))) continue;
		if (source == RAW_IMU) raw_base = count;
		const struct capture_fields *kind = &field_kinds[source_kinds[source]];
		for (unsigned int field = 0; field < kind->wire_count; field++) {
			const struct capture_wire_field *entry = &kind->wire[field];
			if (entry->selected >= 0 && !(fields[source] & BIT(entry->selected))) continue;
			int alias = source == CALIBRATED_IMU && raw_base >= 0 && field < CAPTURE_IMU_SHARED_FIELDS ?
				raw_base + field : -1;
			if (wire && count == capacity) return 0;
			if (wire) wire[count] = (struct telemetry_wire_field){offset, entry->size, alias, entry->delta};
			count++;
			offset += entry->size;
		}
	}
	return offset == row_size ? count : 0;
}

int capture_wire_layout(const uint8_t config[CONFIG_SIZE], size_t row_size,
	struct telemetry_wire_field *wire, size_t capacity)
{
	uint32_t mask = sys_get_le32(config);
	if (!mask || mask >> SOURCE_COUNT || !sys_get_le32(config + 4) ||
	    !sys_get_le32(config + 12)) return -EINVAL;
	for (unsigned int source = 0; source < SOURCE_COUNT; source++) {
		uint8_t selected = config[16 + source];
		if (!(mask & BIT(source))) {
			if (selected) return -EINVAL;
			continue;
		}
		const struct capture_fields *kind = &field_kinds[source_kinds[source]];
		if (!selected || selected & ~((1U << kind->count) - 1)) return -EINVAL;
	}
	size_t count = wire_layout(mask, config + 16, row_size, wire, capacity);
	return count ? count : -EINVAL;
}

int capture_app_wire_layout(const uint8_t *config, size_t config_size, size_t row_size,
	struct telemetry_wire_field *wire, size_t capacity)
{
	if (!config || config_size <= BP_CAPTURE_CONFIG_SIZE + 1 ||
	    config_size > BP_CAPTURE_APP_MAX_CONFIG_SIZE) return -EINVAL;
	unsigned int count = config[BP_CAPTURE_CONFIG_SIZE];
	if (!count || count > BP_CAPTURE_APP_MAX_FIELDS) return -EINVAL;
	size_t names[BP_CAPTURE_APP_MAX_FIELDS], lengths[BP_CAPTURE_APP_MAX_FIELDS];
	uint8_t sizes[BP_CAPTURE_APP_MAX_FIELDS], deltas[BP_CAPTURE_APP_MAX_FIELDS];
	size_t offset = BP_CAPTURE_CONFIG_SIZE + 1, app_size = 0;
	for (unsigned int field = 0; field < count; field++) {
		if (offset + 2 > config_size) return -EINVAL;
		uint8_t type = config[offset++], length = config[offset++];
		if (!length || length > BP_CAPTURE_APP_MAX_NAME || offset + length > config_size)
			return -EINVAL;
		if (!((config[offset] >= 'A' && config[offset] <= 'Z') ||
		      (config[offset] >= 'a' && config[offset] <= 'z'))) return -EINVAL;
		for (unsigned int letter = 1; letter < length; letter++) {
			uint8_t ch = config[offset + letter];
			if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
			      (ch >= '0' && ch <= '9') || ch == '_')) return -EINVAL;
		}
		for (unsigned int prior = 0; prior < field; prior++) {
			if (length == lengths[prior] &&
			    !memcmp(config + offset, config + names[prior], length)) return -EINVAL;
		}
		names[field] = offset;
		lengths[field] = length;
		offset += length;
		sizes[field] = type == 'b' || type == 'B' ? 1 :
			type == 'h' || type == 'H' ? 2 :
			type == 'i' || type == 'I' || type == 'f' ? 4 : 0;
		if (!sizes[field]) return -EINVAL;
		deltas[field] = type == 'i' || type == 'I';
		app_size += sizes[field];
	}
	if (offset != config_size || row_size < app_size || (wire && capacity < count))
		return -EINVAL;
	int base_count = capture_wire_layout(config, row_size - app_size, wire,
		wire ? capacity - count : 0);
	if (base_count < 0) return base_count;
	if (wire && (capacity < count || (size_t)base_count > capacity - count)) return -EINVAL;
	if (wire) {
		size_t position = row_size - app_size;
		for (unsigned int field = 0; field < count; field++) {
			wire[base_count + field] = (struct telemetry_wire_field){
				position, sizes[field], -1, deltas[field]};
			position += sizes[field];
		}
	}
	return base_count + count;
}

static int block_start(struct capture_task *run)
{
	size_t prefix = plan.step_count ? 4 : 0;
	uint32_t slots;
	size_t room = application_record_room(&slots);
	if (!slots || room < prefix + 8 + plan.snapshot_size) return -ENOSPC;
	if (prefix) sys_put_le32(4, run->block_data);
	return telemetry_block_init(&run->block, run->block_data + prefix,
		MIN(sizeof(run->block_data) - prefix, room - prefix), run->data + 1024,
		plan.snapshot_size, run->wire, run->field_count, false);
}

static int block_flush(struct capture_task *run)
{
	if (!run->block.rows) {
		run->block = (struct telemetry_block){0};
		return 0;
	}
	size_t prefix = plan.step_count ? 4 : 0;
	int err = application_record_write(run->block_data, prefix + run->block.length);
	if (!err) run->block = (struct telemetry_block){0};
	return err;
}

static int block_append(struct capture_task *run, const uint8_t *row)
{
	int err = run->block.data ? 0 : block_start(run);
	if (err) return err;
	err = telemetry_block_append(&run->block, row);
	if (err != -ENOSPC) return err;
	err = block_flush(run);
	if (err) return err;
	err = block_start(run);
	return err ? err : telemetry_block_append(&run->block, row);
}

static void finish_passive(struct capture_task *run, uint32_t reason)
{
	int err = block_flush(run);
	if (err) {
		atomic_set(&failure, err);
		reason = BP_RECORD_ERROR;
	}
	application_record_finish(reason);
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
			finish_passive(run, BP_RECORD_CANCELLED);
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
			atomic_add(&missed, plan.samples - slot);
			finish_passive(run, BP_RECORD_DURATION);
			return;
		}
		slot = current;
		uint8_t *out = run->data;
		put64(&out, origin_us + slot * period);
		put64(&out, now);
		put64(&out, 0); /* Completion is published only with the whole sample. */
		put32(&out, skipped);
		observe(run, out);
		sys_put_le64(hub_time_us(), run->data + 16);
		int err = block_append(run, run->data);
		if (err) {
			if (err != -ENOSPC || plan.duration_ms) {
				atomic_set(&failure, err);
				application_record_finish(BP_RECORD_ERROR);
			} else {
				application_record_finish(BP_RECORD_FULL);
			}
			return;
		}
		run->written++;
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
	int err = block_flush(run);
	if (err) return err;
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
			/* Expiry is normal only after the whole command reached the outputs.
			 * A request that expired in the queue is a missed action. */
			if (!err && state.error && !(state.error == -ETIMEDOUT &&
			    state.applied_mask == plan.port_mask && now >= action_end(run))) err = state.error;
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
			observe(run, out);
			sys_put_le64(hub_time_us(), run->data + 20);
			err = block_append(run, run->data + 4);
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
	result = block_flush(run);
	if (result && !err) err = result;
	if (err) reason = BP_RECORD_ERROR;
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
	shell_print(shell, "Blocks: %u bytes maximum; %u slots; %zu payload bytes; %zu RAM bytes",
		plan.record.size, plan.record.capacity, plan.payload_bytes, plan.record_memory);
	shell_print(shell, "Samples: %u raw bytes; raw rate: %" PRIu64 " bytes/s; guaranteed window: %" PRIu64 " ms",
		plan.snapshot_size,
		DIV_ROUND_UP((uint64_t)plan.snapshot_size * 1000, plan.period_ms),
		plan.duration_ms ? (uint64_t)plan.duration_ms : (uint64_t)plan.samples * plan.period_ms);
	shell_print(shell, "Worker: %zu bytes + %zu stack bytes; allocator overhead separate",
		sizeof(struct capture_task) + 1024 + plan.snapshot_size, (size_t)K_THREAD_STACK_LEN(STACK_SIZE));
	struct application_memory memory;
	if (!application_memory_get(&memory)) {
		shell_print(shell, "Shared heap free: %zu bytes; start still requires allocation", memory.heap_free);
	}
	for (unsigned int i = 0; i < SOURCE_COUNT; i++) {
		if (plan.mask & BIT(i)) show_source(shell, i, plan.fields[i]);
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
	if (candidate->snapshot_size + 8 + (count ? 4 : 0) > 1024) return -EMSGSIZE;
	/* A one-row block is the largest possible cost per accepted sample.
	 * Interleaved motor events may force that boundary on every row. */
	size_t fields = 4;
	for (unsigned int source = 0; source < SOURCE_COUNT; source++) {
		if (!(candidate->mask & BIT(source))) continue;
		const struct capture_fields *kind = &field_kinds[source_kinds[source]];
		for (unsigned int field = 0; field < kind->wire_count; field++) {
			int selected = kind->wire[field].selected;
			fields += selected < 0 || (candidate->fields[source] & BIT(selected)) != 0;
		}
	}
	size_t sample_bytes = candidate->snapshot_size + MAX(8 + (count ? 4 : 0),
		DIV_ROUND_UP(fields, 8));
	size_t group_bytes = 56 + 40 * POPCOUNT(candidate->port_mask);
	candidate->record.size = 1024;
	strcpy(candidate->record.schema, count ? "brickphyr.bench/4" : "brickphyr.capture/3");
	uint32_t events = count ? 2 * count + 2 : 0;
	size_t event_bytes = count ? 2 * count * group_bytes + 12 + 36 : 0;
	uint32_t capacity = (32768 - config_size) / sample_bytes;
	/* Include payload, offset index and allocation pointers. A conservative
	 * bound guarantees even an incompressible sample can be retained. */
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
	candidate->payload_bytes = samples * sample_bytes + event_bytes;
	return application_record_compact_budget(candidate->record.size, candidate->record.capacity,
		config_size, candidate->payload_bytes, &candidate->record_memory);
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
		.record = {.size = HEADER_SIZE}};
	for (size_t arg = 3; arg < argc; arg++) {
		if (!strcmp(argv[arg], "all")) {
			if (argc != 4) return -EINVAL;
			for (unsigned int i = 0; i < SOURCE_COUNT; i++) {
				if (!available(i)) continue;
				err = select_source(&candidate, sources[i]);
				if (err) return err;
			}
			break;
		}
		err = select_source(&candidate, argv[arg]);
		if (err) return err;
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
	struct capture_task *run = k_calloc(1, sizeof(*run) + 1024 + plan.snapshot_size);
	if (!run) return -ENOMEM;
	run->epoch = expected;
	run->stack = k_thread_stack_alloc(STACK_SIZE, 0);
	if (!run->stack) {
		k_free(run);
		return -ENOMEM;
	}
	run->field_count = wire_layout(plan.mask, plan.fields, plan.snapshot_size,
		run->wire, ARRAY_SIZE(run->wire));
	if (!run->field_count) {
		(void)k_thread_stack_free(run->stack);
		k_free(run);
		return -EINVAL;
	}
	uint8_t *out = run->data;
	put32(&out, plan.mask);
	put32(&out, plan.period_ms);
	put32(&out, plan.duration_ms);
	put32(&out, HUB_TIME_RESOLUTION_US);
	memcpy(out, plan.fields, sizeof(plan.fields));
	out += sizeof(plan.fields);
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
			if (available(i)) show_source(shell, i, BIT(field_kinds[source_kinds[i]].count) - 1);
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
	SHELL_CMD_ARG(sources, NULL, "List available sources, fields and record sizes.", command, 1, 0),
	SHELL_CMD_ARG(prepare, NULL, "period_ms duration_ms|full all|source[:field,...] ...", command, 4, SOURCE_COUNT - 1),
	SHELL_CMD_ARG(at, NULL, "offset_ms duration_ms A=duty|brake|coast ...", command, 4, BP_PORT_COUNT - 1),
	SHELL_CMD_ARG(check, NULL, "Validate prepared ports and show the plan without actuation.", command, 1, 0),
	SHELL_CMD_ARG(start, NULL, "Allocate and start the prepared capture/schedule.", command, 1, 0),
	SHELL_CMD_ARG(status, NULL, "Show plan, memory and retained capture.", command, 1, 0),
	SHELL_CMD_ARG(stop, NULL, "Stop sampling and scheduled outputs; retain records.", command, 1, 0),
	SHELL_CMD_ARG(clear, NULL, "Discard stopped capture and release its power hold.", command, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(capture, &commands, "Finite RAM observations and explicitly started motor schedules.", NULL);
