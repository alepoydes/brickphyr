// SPDX-License-Identifier: Apache-2.0
#include "application_record.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

enum { MAX_RECORD_SIZE = 904, PARAMETER_SIZE = 60, CONFIG_SIZE = 504 };

static void *held[32];
static uint8_t record[MAX_RECORD_SIZE], output[MAX_RECORD_SIZE];
static uint8_t parameters[PARAMETER_SIZE], copied_parameters[PARAMETER_SIZE];
static uint8_t config[CONFIG_SIZE], copied_config[CONFIG_SIZE];

static void fill_record(unsigned int index, size_t size)
{
	for (size_t i = 0; i < size; i++) record[i] = (uint8_t)(index + 3 * i);
}

static void fill_parameters(unsigned int revision)
{
	for (size_t i = 0; i < sizeof(parameters); i++) parameters[i] = (uint8_t)(revision + i);
}

static void exercise(const struct bp_record_spec *spec, bool control,
		     size_t parameter_size, size_t config_size, unsigned int extra)
{
	for (size_t i = 0; i < config_size; i++) config[i] = (uint8_t)(i * 7);
	application_record_reset();
	zassert_ok(application_record_begin(spec, config_size ? config : NULL, config_size,
		control, parameter_size));
	struct bp_record_info info;
	application_record_info(&info);
	uint32_t generation = info.generation;
	for (unsigned int index = 0; index < spec->capacity + extra; index++) {
		unsigned int revision = 1 + index / 7;
		fill_record(index, spec->size);
		if (control) {
			fill_parameters(revision);
			struct bp_control_step step = {
				.sequence = index + 1, .parameter_revision = revision,
				.parameter_size = parameter_size,
			};
			application_record_step(&step, parameters);
		}
		zassert_ok(application_record_write(record, spec->size));
	}
	application_record_info(&info);
	zassert_equal(info.first, extra);
	zassert_equal(info.next, spec->capacity + extra);
	zassert_equal(application_record_copy(generation, extra - 1, output, sizeof(output),
		copied_parameters, sizeof(copied_parameters), &info, &(struct bp_control_step){0}),
		-EOVERFLOW);
	if (config_size) {
		zassert_equal(application_record_config_read(generation, 0, copied_config,
			sizeof(copied_config), &info), config_size);
		zassert_mem_equal(copied_config, config, config_size);
	}
	for (unsigned int index = extra; index < spec->capacity + extra; index++) {
		struct bp_control_step step;
		zassert_equal(application_record_copy(generation, index, output, sizeof(output),
			copied_parameters, parameter_size, &info, &step), spec->size);
		fill_record(index, spec->size);
		zassert_mem_equal(output, record, spec->size);
		if (control) {
			fill_parameters(1 + index / 7);
			zassert_mem_equal(copied_parameters, parameters, parameter_size);
			zassert_equal(step.sequence, index + 1);
			zassert_equal(step.parameter_revision, 1 + index / 7);
		}
	}
}

ZTEST(record_memory, test_fragmented_heap_and_reclaimed_begin)
{
	const struct bp_record_spec large_spec = {
		.size = 904, .capacity = 31, .schema = "fragmented-recorder/1",
	};
	const struct bp_record_spec packed_spec = {
		.size = 173, .capacity = 31, .schema = "packed-recorder/1",
	};
	const struct bp_record_spec tiny_spec = {
		.size = 1, .capacity = 1025, .schema = "tiny-recorder/1",
	};
	application_record_reset();

	/* Occupy the heap completely, then expose only separated 4 KiB holes. */
	size_t count = 0;
	while (count < ARRAY_SIZE(held) && (held[count] = k_malloc(4096))) count++;
	zassert_true(count >= 16 && count < ARRAY_SIZE(held), "unexpected heap capacity: %zu", count);
	const size_t holes[] = {1, 4, 7};
	for (size_t i = 0; i < ARRAY_SIZE(holes); i++) {
		k_free(held[holes[i]]);
		held[holes[i]] = NULL;
	}
	zassert_equal(application_record_begin(&large_spec, config, sizeof(config), true, PARAMETER_SIZE),
		-ENOMEM, "begin should fail after some block allocations");
	struct bp_record_info info;
	application_record_info(&info);
	zassert_equal(info.spec.capacity, 0, "failed begin published a recording");
	zassert_equal(application_record_copy(info.generation, 0, output, sizeof(output),
		copied_parameters, sizeof(copied_parameters), &info, &(struct bp_control_step){0}),
		-ENODATA);
	for (size_t i = 0; i < ARRAY_SIZE(holes); i++) {
		held[holes[i]] = k_malloc(4096);
		zassert_not_null(held[holes[i]], "failed begin retained heap memory");
	}

	/* Aggregate free space suffices, but no 30 KiB contiguous request does. */
	for (size_t i = 0; i < count; i += 2) {
		k_free(held[i]);
		held[i] = NULL;
	}
	void *large = k_malloc(30 * 1024);
	zassert_is_null(large, "heap was not fragmented");
	exercise(&large_spec, true, PARAMETER_SIZE, CONFIG_SIZE, 16);

	application_record_reset();
	application_record_info(&info);
	zassert_equal(info.spec.capacity, 0);
	zassert_ok(application_record_begin(&large_spec, config, sizeof(config), true, PARAMETER_SIZE),
		"reset did not release recorder blocks");
	application_record_clear();
	for (size_t i = 0; i < count; i++) k_free(held[i]);
	large = k_malloc(30 * 1024);
	zassert_not_null(large, "recorder did not release fragmented heap memory");
	k_free(large);

	/* Two control slots per block, then a partial final block. */
	exercise(&packed_spec, true, PARAMETER_SIZE, CONFIG_SIZE, 16);
	application_record_clear();
	/* 1024 tiny slots in one block, plus a one-slot tail without control metadata. */
	exercise(&tiny_spec, false, 0, 0, 8);
	application_record_clear();
}

ZTEST(record_memory, test_blocks_cross_slots_headers_and_ring_wrap)
{
	const struct bp_record_spec spec = {.size = 5, .capacity = 3, .schema = "blocks/1"};
	for (int mode = 0; mode < 2; mode++) {
		bool control = mode != 0;
		application_record_reset();
		zassert_ok(application_record_begin(&spec, NULL, 0, control, 0));
		uint8_t expected[3 * (32 + 5)], actual[sizeof(expected)] = {0};
		size_t stride = 5 + (control ? 32 : 0);
		for (size_t i = 0; i < 5; i++) {
			struct bp_control_step step = {.sequence = i + 1, .parameter_revision = i};
			const uint8_t payload[] = {i, 0, 255, i + 1, 9};
			if (control) application_record_step(&step, NULL);
			zassert_ok(application_record_write(payload, sizeof(payload)));
			if (i >= 2) {
				uint8_t *target = expected + (i - 2) * stride;
				if (control) memcpy(target, &step, 32);
				memcpy(target + stride - 5, payload, 5);
			}
		}
		struct bp_record_info info;
		application_record_info(&info);
		uint32_t generation = info.generation;
		for (size_t offset = 0; offset < stride * 3;) {
			int length = application_record_read_block(generation, 2 + offset / stride,
				offset % stride, actual + offset, 13, &info);
			zassert_true(length > 0 && length <= 13);
			offset += length;
		}
		zassert_mem_equal(actual, expected, stride * 3);
		zassert_equal(application_record_read_block(generation, 5, 0, actual, 13, &info), 0);
		zassert_equal(application_record_read_block(generation, 1, 0, actual, 13, &info), 0);
		zassert_equal(info.first, 2);
		zassert_equal(application_record_read_block(generation, 2, stride, actual, 13, &info), -EINVAL);
		application_record_clear();
	}
}

ZTEST(record_memory, test_paused_control_stream_retains_only_unacknowledged_steps)
{
	const struct bp_record_spec spec = {.size = 2, .capacity = 3, .schema = "paused/1"};
	struct bp_record_info info;
	struct bp_control_step step;
	uint8_t values[2], applied[4], parameter[4];
	application_record_reset();
	zassert_ok(application_record_begin_paused(&spec, NULL, 0, true, sizeof(parameter)));
	application_record_info(&info);
	uint32_t generation = info.generation;
	for (unsigned int i = 0; i < 3; i++) {
		memset(parameter, i + 1, sizeof(parameter));
		step = (struct bp_control_step){.sequence = i + 1,
			.parameter_revision = i + 1, .parameter_size = sizeof(parameter)};
		application_record_step(&step, parameter);
		values[0] = i;
		zassert_ok(application_record_write(values, sizeof(values)));
	}
	values[0] = 99;
	zassert_equal(application_record_write(values, sizeof(values)), -ENOSPC);
	application_record_info(&info);
	zassert_true(info.active);
	zassert_equal(info.first, 0);
	zassert_equal(info.next, 3);
	zassert_equal(application_record_ack(generation + 1, 2, 1), -ESTALE);
	zassert_equal(application_record_ack(generation, 4, 1), -EINVAL);
	zassert_ok(application_record_ack(generation, 2, 1));
	zassert_equal(application_record_ack(generation, 3, 2), -EBUSY);
	application_record_session_closed(1);
	zassert_ok(application_record_ack(generation, 2, 2));
	for (unsigned int i = 3; i < 5; i++) {
		memset(parameter, i + 1, sizeof(parameter));
		step = (struct bp_control_step){.sequence = i + 1,
			.parameter_revision = i + 1, .parameter_size = sizeof(parameter)};
		application_record_step(&step, parameter);
		values[0] = i;
		zassert_ok(application_record_write(values, sizeof(values)));
	}
	for (unsigned int i = 2; i < 5; i++) {
		zassert_equal(application_record_copy(generation, i, values, sizeof(values), applied,
			sizeof(applied), &info, &step), sizeof(values));
		zassert_equal(values[0], i);
		zassert_equal(step.sequence, i + 1);
		for (size_t j = 0; j < sizeof(applied); j++) zassert_equal(applied[j], i + 1);
	}
	zassert_ok(application_record_ack(generation, 5, 2));
	/* The last revision's slot was retired too; next insertion must not reuse it. */
	memset(parameter, 5, sizeof(parameter));
	step = (struct bp_control_step){.sequence = 6, .parameter_revision = 5,
		.parameter_size = sizeof(parameter)};
	application_record_step(&step, parameter);
	values[0] = 5;
	zassert_ok(application_record_write(values, sizeof(values)));
	zassert_equal(application_record_copy(generation, 5, values, sizeof(values), applied,
		sizeof(applied), &info, &step), sizeof(values));
	zassert_equal(values[0], 5);
	zassert_equal(step.sequence, 6);
	for (size_t j = 0; j < sizeof(applied); j++) zassert_equal(applied[j], 5);
	application_record_clear();
}

ZTEST(record_memory, test_variable_paused_blocks_wrap_without_padding_on_wire)
{
	const struct bp_record_spec spec = {.size = 5, .capacity = 3, .schema = "variable/1"};
	const uint8_t data[] = {10, 11, 12, 13, 14};
	uint8_t actual[2 + 4 + 2 + 1 + 2 + 5];
	const uint8_t expected[] = {4, 0, 10, 11, 12, 13, 1, 0, 10,
		5, 0, 10, 11, 12, 13, 14};
	struct bp_record_info info;
	bool variable;
	application_record_reset();
	zassert_ok(application_record_begin_variable_paused(&spec, NULL, 0));
	application_record_snapshot(&info, NULL, &variable, NULL, NULL);
	zassert_true(variable);
	uint32_t generation = info.generation;
	for (size_t length = 2; length <= 4; length++) {
		zassert_ok(application_record_write(data, length));
	}
	zassert_equal(application_record_write(data, 1), -ENOSPC);
	zassert_ok(application_record_ack(generation, 2, 1));
	zassert_ok(application_record_write(data, 1));
	zassert_ok(application_record_write(data, 5));
	zassert_equal(application_record_read_block(generation, 2, 0, actual,
		sizeof(actual), &info), sizeof(actual));
	zassert_mem_equal(actual, expected, sizeof(expected));
	zassert_equal((info.first), 2);
	zassert_equal((info.next), 5);
	application_record_clear();
}

ZTEST(record_memory, test_segments_resume_only_after_previous_series_is_drained)
{
	const struct bp_record_spec spec = {.size = 2, .capacity = 3, .schema = "segments/1"};
	struct bp_record_info info;
	struct bp_control_step step;
	uint8_t value[2] = {0}, applied[4], parameter[4] = {7};
	bool paused, segments;
	application_record_reset();
	zassert_ok(application_record_begin_segments(&spec, NULL, 0, true, sizeof(parameter)));
	application_record_snapshot(&info, NULL, NULL, &paused, &segments);
	zassert_false(paused);
	zassert_true(segments);
	uint32_t generation = info.generation;
	for (unsigned int i = 1; i <= 3; i++) {
		step = (struct bp_control_step){.sequence = i, .parameter_revision = 1,
			.parameter_size = sizeof(parameter)};
		application_record_step(&step, parameter);
		value[0] = i;
		zassert_ok(application_record_write(value, sizeof(value)));
	}
	zassert_equal(application_record_write(value, sizeof(value)), -ENOSPC);
	zassert_equal(application_record_ack(generation, 3, 1), -ENOTSUP);
	zassert_ok(application_record_advance(generation, 0, 0));
	zassert_equal(application_record_advance(generation, 1, 2), -EINVAL);
	application_record_info(&info);
	zassert_equal(info.first, 0);
	zassert_ok(application_record_advance(generation, 0, 2));
	zassert_equal(application_record_write(value, sizeof(value)), -ENOSPC);
	zassert_equal(application_record_copy(generation, 2, value, sizeof(value), applied,
		sizeof(applied), &info, &step), sizeof(value));
	zassert_equal(step.sequence, 3);
	zassert_mem_equal(applied, parameter, sizeof(applied));
	zassert_ok(application_record_advance(generation, 2, 3));
	step = (struct bp_control_step){.sequence = 9, .parameter_revision = 2,
		.parameter_size = sizeof(parameter)};
	application_record_step(&step, parameter);
	zassert_ok(application_record_write(value, sizeof(value)));
	application_record_info(&info);
	zassert_equal(info.first, 3);
	zassert_equal(info.next, 4);
	application_record_clear();
}

ZTEST(record_memory, test_capture_tag_is_bound_once_and_expires_with_data)
{
	const struct bp_record_spec spec = {.size = 1, .capacity = 1, .schema = "tag/1"};
	const uint8_t original[16] = {1}, other[16] = {2}, empty[16] = {0};
	uint8_t tag[16];
	struct bp_record_info info;
	application_record_reset();
	zassert_ok(application_record_begin(&spec, NULL, 0, false, 0));
	application_record_snapshot(&info, tag, NULL, NULL, NULL);
	zassert_mem_equal(tag, empty, sizeof(tag));
	uint32_t generation = info.generation;
	zassert_equal(application_record_bind(generation, empty, tag), -EINVAL);
	zassert_ok(application_record_bind(generation, original, tag));
	zassert_mem_equal(tag, original, sizeof(tag));
	application_record_end();
	zassert_ok(application_record_bind(generation, other, tag));
	zassert_mem_equal(tag, original, sizeof(tag));
	application_record_reset();
	zassert_ok(application_record_begin(&spec, NULL, 0, false, 0));
	zassert_equal(application_record_bind(generation, original, tag), -ESTALE);
	application_record_snapshot(&info, tag, NULL, NULL, NULL);
	zassert_mem_equal(tag, empty, sizeof(tag));
	zassert_ok(application_record_bind(info.generation, other, tag));
	zassert_mem_equal(tag, other, sizeof(tag));
	application_record_clear();
}

ZTEST(record_memory, test_finite_capture_freezes_without_overwriting)
{
	const struct bp_record_spec spec = {.size = 2, .capacity = 3, .schema = "finite/1"};
	const uint8_t expected[] = {1, 2, 3, 4, 5, 6};
	const uint32_t reasons[] = {BP_RECORD_FULL, BP_RECORD_DURATION};
	uint8_t actual[sizeof(expected)];
	struct bp_record_info info;
	application_record_reset();
	zassert_equal(application_record_begin_finite(&spec, NULL, 0, BP_RECORD_CANCELLED), -EINVAL);
	application_record_info(&info);
	zassert_equal(info.spec.capacity, 0);
	for (size_t mode = 0; mode < ARRAY_SIZE(reasons); mode++) {
		application_record_reset();
		zassert_ok(application_record_begin_finite(&spec, NULL, 0, reasons[mode]));
		application_record_info(&info);
		uint32_t generation = info.generation;
		for (size_t index = 0; index < spec.capacity; index++) {
			zassert_ok(application_record_write(expected + index * spec.size, spec.size));
		}
		application_record_info(&info);
		zassert_false(info.active);
		zassert_equal(info.reason, reasons[mode]);
		zassert_equal(info.first, 0);
		zassert_equal(info.next, spec.capacity);
		zassert_equal(application_record_write(expected, spec.size), -ENOSPC);
		application_record_finish(BP_RECORD_ERROR);
		zassert_equal(application_record_read_block(generation, 0, 0, actual, sizeof(actual), &info),
			sizeof(actual));
		zassert_mem_equal(actual, expected, sizeof(expected));
		zassert_equal(info.reason, reasons[mode]);
		zassert_equal(info.first, 0);
		zassert_equal(info.next, spec.capacity);
	}

	application_record_reset();
	zassert_ok(application_record_begin_finite(&spec, NULL, 0, BP_RECORD_DURATION));
	zassert_ok(application_record_write(expected, spec.size));
	application_record_finish(BP_RECORD_DURATION);
	application_record_finish(BP_RECORD_CANCELLED);
	application_record_info(&info);
	zassert_false(info.active);
	zassert_equal(info.reason, BP_RECORD_DURATION);
	zassert_equal(info.next, 1);

	application_record_reset();
	zassert_ok(application_record_begin(&spec, NULL, 0, false, 0));
	for (size_t index = 0; index < spec.capacity + 1; index++) {
		zassert_ok(application_record_write(expected, spec.size));
	}
	application_record_info(&info);
	zassert_equal(info.first, 1, "Reset left the finite mode enabled");
	zassert_equal(info.next, spec.capacity + 1);
	application_record_clear();
}

ZTEST(record_memory, test_compact_capture_crosses_blocks_and_resumes_mid_prefix)
{
	const struct bp_record_spec spec = {.size = 904, .capacity = 5, .schema = "variable/1"};
	const size_t lengths[] = {3, 904, 173, 904, 12};
	uint8_t expected[2006], actual[2006];
	size_t payload = 0, bytes, end = 0;
	for (size_t i = 0; i < ARRAY_SIZE(lengths); i++) payload += lengths[i];
	zassert_ok(application_record_compact_budget(spec.size, spec.capacity, 0, payload, &bytes));
	application_record_reset();
	zassert_ok(application_record_begin_compact(&spec, NULL, 0, payload, BP_RECORD_FULL));
	zassert_equal(application_record_memory(), bytes);
	struct bp_record_info info;
	bool variable;
	application_record_snapshot(&info, NULL, &variable, NULL, NULL);
	zassert_true(variable);
	uint32_t generation = info.generation;
	for (size_t i = 0; i < ARRAY_SIZE(lengths); i++) {
		fill_record(i, lengths[i]);
		sys_put_le16(lengths[i], expected + end);
		memcpy(expected + end + 2, record, lengths[i]);
		end += lengths[i] + 2;
		if (i + 1 == ARRAY_SIZE(lengths)) {
			/* Exhaustion rejects the whole write without changing the prefix;
			 * the reserved final event still fits and keeps its actual reason. */
			zassert_equal(application_record_write(record, 13), -ENOSPC);
			zassert_ok(application_record_write_final(record, lengths[i], BP_RECORD_CANCELLED));
		} else zassert_ok(application_record_write(record, lengths[i]));
	}
	zassert_equal(end, sizeof(expected));
	size_t index = 0, offset = 0, position = 0;
	while (position < end) {
		int length = application_record_read_block(generation, index, offset, actual + position,
			MIN(position ? 13 : 1, end - position), &info);
		zassert_true(length > 0);
		position += length;
		offset += length;
		while (index < ARRAY_SIZE(lengths) && offset >= lengths[index] + 2) {
			offset -= lengths[index++] + 2;
		}
	}
	zassert_mem_equal(actual, expected, end);
	zassert_false(info.active);
	zassert_equal(info.reason, BP_RECORD_CANCELLED);
	zassert_equal(info.next, spec.capacity);
	zassert_equal(application_record_read(generation, 2, 172, output, sizeof(output),
		&info, &(struct bp_control_step){0}), 1);
	zassert_equal(output[0], (uint8_t)(2 + 3 * 172));
	zassert_equal(application_record_read_block(generation, 2, 175, output, 1, &info), -EINVAL);
	application_record_clear();
	application_record_snapshot(&info, NULL, &variable, NULL, NULL);
	zassert_false(variable);
	zassert_equal(application_record_memory(), 0);
}

ZTEST_SUITE(record_memory, NULL, NULL, NULL, NULL, NULL);
