// SPDX-License-Identifier: Apache-2.0
#define __ZEPHYR_SUPERVISOR__
#include "application.h"
#include "application_record.h"
#include "native_image.h"
#include "app_store.h"
#include "python_source.h"
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt.h>
#endif

#include <brickphyr_abi.h>
#include <errno.h>
#include <string.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

/* Experimental Brickphyr application group; framing and transports remain SMP. */
#define APPLICATION_GROUP 65
BUILD_ASSERT(CONFIG_MCUMGR_SMP_CBOR_MAX_MAIN_MAP_ENTRIES >= 24);
/* Stream response: 256 data bytes, seven bounded status fields and SMP/CBOR
 * headers fit in 384 bytes, including worst-case 32-bit result/counter values. */
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE >= 384);

static int result(struct smp_streamer *stream, int err)
{
	return !err || smp_add_cmd_err(stream->writer->zs, APPLICATION_GROUP, -err) ?
		MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static bool number(zcbor_state_t *zs, const char *key, int64_t value)
{
	return zcbor_tstr_encode_ptr(zs, key, strlen(key)) && zcbor_int64_put(zs, value);
}

static int status_read(struct smp_streamer *stream)
{
	struct application_status status;
	application_get_status(&status);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = number(zs, "api", 1) &&
		zcbor_tstr_put_lit(zs, "abi") && zcbor_tstr_put_lit(zs, BRICKPHYR_ABI) &&
		number(zs, "state", status.state) && number(zs, "result", status.result) &&
		number(zs, "fault", status.fault) && number(zs, "cleanup", status.cleanup_error) &&
		number(zs, "image", status.image_bytes) && number(zs, "heap", status.loader_bytes) &&
		number(zs, "loaded", status.loaded_bytes) && number(zs, "stack", status.stack_bytes) &&
		number(zs, "unused", status.stack_unused) && number(zs, "dropped", status.output_dropped) &&
		number(zs, "control_stack", status.control_stack_bytes) &&
		number(zs, "control_unused", status.control_stack_unused) &&
		number(zs, "queued", status.output_bytes) &&
		number(zs, "received", status.upload_bytes) && number(zs, "total", status.upload_size) &&
		zcbor_tstr_put_lit(zs, "ready") && zcbor_bool_put(zs, status.upload_ready) &&
		number(zs, "limit", APPLICATION_IMAGE_LIMIT) && number(zs, "format", status.format) &&
		number(zs, "formats", IS_ENABLED(CONFIG_BRICKPHYR_PYTHON) ? 15 : 1) &&
		number(zs, "code", status.code_bytes) && number(zs, "code_used", status.code_used) &&
		zcbor_tstr_put_lit(zs, "repl") && zcbor_bool_put(zs, status.repl);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int upload_write(struct smp_streamer *stream)
{
	uint32_t offset = UINT32_MAX, total = 0;
	uint32_t format = APPLICATION_NATIVE;
	struct zcbor_string data = {0};
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &offset),
		ZCBOR_MAP_DECODE_KEY_DECODER("total", zcbor_uint32_decode, &total),
		ZCBOR_MAP_DECODE_KEY_DECODER("data", zcbor_bstr_decode, &data),
		ZCBOR_MAP_DECODE_KEY_DECODER("format", zcbor_uint32_decode, &format),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    offset == UINT32_MAX || !total || !data.value) {
		return MGMT_ERR_EINVAL;
	}
	if (format > APPLICATION_FILES) {
		return result(stream, -ENOTSUP);
	}
	size_t next;
	int err = application_upload(format, offset, total, data.value, data.len, &next);
	if (err) {
		return result(stream, err);
	}
	return number(stream->writer->zs, "off", next) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int run_write(struct smp_streamer *stream)
{
	uint32_t stack = 0;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("stack", zcbor_uint32_decode, &stack),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded)) return MGMT_ERR_EINVAL;
	return result(stream, application_run_uploaded(false, stack));
}

static int memory_read(struct smp_streamer *stream)
{
	struct application_memory value;
	int err = application_memory_get(&value);
	if (err) return result(stream, err);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = number(zs, "heap_total", value.heap_total) && number(zs, "heap_used", value.heap_used) &&
		number(zs, "heap_free", value.heap_free) && number(zs, "heap_peak", value.heap_peak) &&
		number(zs, "source_ram", value.source_ram) && number(zs, "source_flash", value.source_flash) &&
		number(zs, "staging", value.staging) && number(zs, "python_arena", value.python_arena) &&
		number(zs, "primary_native", value.primary_native) && number(zs, "control_native", value.control_native) &&
		number(zs, "code", value.code) && number(zs, "code_used", value.code_used) &&
		number(zs, "records", value.records) && number(zs, "gc_used", value.gc_used) &&
		number(zs, "gc_free", value.gc_free) && number(zs, "gc_age_ms", value.gc_age_ms) &&
		number(zs, "stack_min", value.stack_min) && number(zs, "stack_max", value.stack_max) &&
		number(zs, "stack_default", value.stack_default) &&
		zcbor_tstr_put_lit(zs, "active") && zcbor_bool_put(zs, value.active);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int stack_read(struct smp_streamer *stream)
{
	uint32_t index = 0;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("index", zcbor_uint32_decode, &index),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded)) return MGMT_ERR_EINVAL;
	struct application_stack_info value;
	int err = application_stack_get(index, &value);
	if (err) return result(stream, err);
	zcbor_state_t *zs = stream->writer->zs;
	if (!zcbor_tstr_put_lit(zs, "present") || !zcbor_bool_put(zs, value.present)) return MGMT_ERR_EMSGSIZE;
	if (!value.present) return MGMT_ERR_EOK;
	static const char *const kinds[] = {"kernel", "primary", "controller"};
	bool ok = zcbor_tstr_put_lit(zs, "name") && zcbor_tstr_encode_ptr(zs, value.name, strlen(value.name)) &&
		zcbor_tstr_put_lit(zs, "kind") && zcbor_tstr_encode_ptr(zs, kinds[value.kind], strlen(kinds[value.kind])) &&
		number(zs, "requested", value.requested) && number(zs, "user", value.user) &&
		number(zs, "unused", value.unused) && number(zs, "privileged", value.privileged) &&
		number(zs, "priv_unused", value.priv_unused) && number(zs, "next", value.next);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int budget_read(struct smp_streamer *stream)
{
	struct zcbor_string name = {0}, resource = {0};
	uint32_t stack = 0, parameters = 0;
	bool control = false;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("name", zcbor_tstr_decode, &name),
		ZCBOR_MAP_DECODE_KEY_DECODER("resource", zcbor_tstr_decode, &resource),
		ZCBOR_MAP_DECODE_KEY_DECODER("stack", zcbor_uint32_decode, &stack),
		ZCBOR_MAP_DECODE_KEY_DECODER("parameters", zcbor_uint32_decode, &parameters),
		ZCBOR_MAP_DECODE_KEY_DECODER("control", zcbor_bool_decode, &control),
	};
	char installed[APP_NAME_MAX + 1], path[PYTHON_PATH_LIMIT + 1];
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    name.len >= sizeof(installed) || resource.len >= sizeof(path) ||
	    (name.len && memchr(name.value, 0, name.len)) ||
	    (resource.len && memchr(resource.value, 0, resource.len))) return MGMT_ERR_EINVAL;
	if (name.len) memcpy(installed, name.value, name.len);
	installed[name.len] = 0;
	if (resource.len) memcpy(path, resource.value, resource.len);
	path[resource.len] = 0;
	struct application_budget_request request = {path, stack, parameters, control};
	struct application_budget value;
	int err;
	if (installed[0]) {
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
		err = app_store_budget(installed, &request, &value);
#else
		err = -ENOTSUP;
#endif
	} else err = application_budget_uploaded(&request, &value);
	if (err) return result(stream, err);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = number(zs, "format", value.format) && number(zs, "image", value.image) &&
		number(zs, "source_ram", value.source_ram) && number(zs, "source_flash", value.source_flash) &&
		number(zs, "transfer", value.transfer) && number(zs, "stack", value.stack) &&
		number(zs, "user_stack", value.user_stack) && number(zs, "priv_stack", value.priv_stack) &&
		number(zs, "arena", value.arena) && number(zs, "code_limit", value.code_limit) &&
		number(zs, "native", value.native) && number(zs, "alignment", value.alignment) &&
		number(zs, "supervisor", value.supervisor);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int record_budget_read(struct smp_streamer *stream)
{
	uint32_t size = 0, capacity = 0, config = 0, parameters = 0;
	bool control = false;
	size_t decoded, bytes;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("size", zcbor_uint32_decode, &size),
		ZCBOR_MAP_DECODE_KEY_DECODER("capacity", zcbor_uint32_decode, &capacity),
		ZCBOR_MAP_DECODE_KEY_DECODER("config", zcbor_uint32_decode, &config),
		ZCBOR_MAP_DECODE_KEY_DECODER("parameters", zcbor_uint32_decode, &parameters),
		ZCBOR_MAP_DECODE_KEY_DECODER("control", zcbor_bool_decode, &control),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded)) return MGMT_ERR_EINVAL;
	int err = application_record_budget(size, capacity, config, control, parameters, &bytes);
	if (err) return result(stream, err);
	return number(stream->writer->zs, "bytes", bytes) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int stop_write(struct smp_streamer *stream)
{
	return result(stream, application_stop());
}

static int output_read(struct smp_streamer *stream)
{
	uint8_t data[128];
	size_t length = application_output(data, sizeof(data));
	bool ok = zcbor_tstr_put_lit(stream->writer->zs, "data") &&
		zcbor_bstr_encode_ptr(stream->writer->zs, data, length);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int stream_read(struct smp_streamer *stream)
{
	uint8_t data[256];
	struct application_status status;
	size_t length = application_read_output(data, sizeof(data), &status);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = zcbor_tstr_put_lit(zs, "data") &&
		zcbor_bstr_encode_ptr(zs, data, length) &&
		number(zs, "state", status.state) && number(zs, "result", status.result) &&
		number(zs, "fault", status.fault) && number(zs, "cleanup", status.cleanup_error) &&
		number(zs, "dropped", status.output_dropped) && number(zs, "queued", status.output_bytes) &&
		zcbor_tstr_put_lit(zs, "repl") && zcbor_bool_put(zs, status.repl);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int discard_write(struct smp_streamer *stream)
{
	application_discard_upload();
	return MGMT_ERR_EOK;
}

static bool record_position(zcbor_state_t *zs, const struct bp_record_info *info)
{
	return number(zs, "generation", info->generation) &&
		zcbor_tstr_put_lit(zs, "first") && zcbor_uint64_put(zs, info->first) &&
		zcbor_tstr_put_lit(zs, "next") && zcbor_uint64_put(zs, info->next) &&
		zcbor_tstr_put_lit(zs, "active") && zcbor_bool_put(zs, info->active) &&
		zcbor_tstr_put_lit(zs, "trigger") &&
		(info->triggered ? zcbor_uint64_put(zs, info->trigger) : zcbor_nil_put(zs, NULL)) &&
		number(zs, "after", info->after) && number(zs, "reason", info->reason);
}

/* Worst-case CBOR position, data header, map terminator and SMP header: 111
 * bytes. Bound the copy/stack even if another service selects large buffers. */
#define RECORD_BLOCK_BYTES MIN(1024, CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - 112)
BUILD_ASSERT(RECORD_BLOCK_BYTES > 0);

static int record_info_read(struct smp_streamer *stream)
{
	struct bp_record_info info;
	uint8_t tag[APPLICATION_RECORD_TAG_SIZE];
	bool variable;
	application_record_snapshot(&info, tag, &variable);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = record_position(zs, &info) && zcbor_tstr_put_lit(zs, "tag") &&
		zcbor_bstr_encode_ptr(zs, tag, sizeof(tag)) && number(zs, "block", RECORD_BLOCK_BYTES) &&
		number(zs, "size", info.spec.size) &&
		number(zs, "config_size", info.config_size) &&
		zcbor_tstr_put_lit(zs, "control") && zcbor_bool_put(zs, info.control) &&
		zcbor_tstr_put_lit(zs, "variable") && zcbor_bool_put(zs, variable) &&
		number(zs, "parameter_size", info.parameter_size) &&
		number(zs, "capacity", info.spec.capacity) && zcbor_tstr_put_lit(zs, "schema") &&
		zcbor_tstr_encode_ptr(zs, info.spec.schema, strlen(info.spec.schema));
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int record_read(struct smp_streamer *stream)
{
	uint32_t generation = 0, offset = UINT32_MAX;
	uint64_t index = UINT64_MAX;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("generation", zcbor_uint32_decode, &generation),
		ZCBOR_MAP_DECODE_KEY_DECODER("index", zcbor_uint64_decode, &index),
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &offset),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !generation || offset == UINT32_MAX || index == UINT64_MAX) return MGMT_ERR_EINVAL;
	uint8_t data[256];
	struct bp_record_info info;
	struct bp_control_step step;
	BUILD_ASSERT(sizeof(step) == 32);
	int length = application_record_read(generation, index, offset, data, sizeof(data), &info, &step);
	if (length < 0) return result(stream, length);
	/* The 32-byte step replaces payload room within the existing SMP budget.
	 * Trim only after the atomic read identifies this generation's format. */
	if (info.control) length = MIN(length, 224);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = record_position(zs, &info) && zcbor_tstr_put_lit(zs, "data") &&
		zcbor_bstr_encode_ptr(zs, data, length) && zcbor_tstr_put_lit(zs, "step") &&
		zcbor_bstr_encode_ptr(zs, (void *)&step, info.control && length ? sizeof(step) : 0);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int record_block_read(struct smp_streamer *stream)
{
	uint32_t generation = 0, offset = UINT32_MAX;
	uint64_t index = UINT64_MAX;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("generation", zcbor_uint32_decode, &generation),
		ZCBOR_MAP_DECODE_KEY_DECODER("index", zcbor_uint64_decode, &index),
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &offset),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !generation || offset == UINT32_MAX || index == UINT64_MAX) return MGMT_ERR_EINVAL;
	uint8_t data[RECORD_BLOCK_BYTES];
	struct bp_record_info info;
	int length = application_record_read_block(generation, index, offset, data, sizeof(data), &info);
	if (length < 0) return result(stream, length);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = record_position(zs, &info) && zcbor_tstr_put_lit(zs, "data") &&
		zcbor_bstr_encode_ptr(zs, data, length);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int record_bind(struct smp_streamer *stream)
{
	uint32_t generation = 0;
	struct zcbor_string candidate = {0};
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("generation", zcbor_uint32_decode, &generation),
		ZCBOR_MAP_DECODE_KEY_DECODER("tag", zcbor_bstr_decode, &candidate),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !generation || candidate.len != APPLICATION_RECORD_TAG_SIZE) return MGMT_ERR_EINVAL;
	uint8_t tag[APPLICATION_RECORD_TAG_SIZE];
	int err = application_record_bind(generation, candidate.value, tag);
	if (err) return result(stream, err);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = number(zs, "generation", generation) && zcbor_tstr_put_lit(zs, "tag") &&
		zcbor_bstr_encode_ptr(zs, tag, sizeof(tag));
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int record_parameters_read(struct smp_streamer *stream)
{
	uint32_t generation = 0, revision;
	uint64_t index = UINT64_MAX;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("generation", zcbor_uint32_decode, &generation),
		ZCBOR_MAP_DECODE_KEY_DECODER("index", zcbor_uint64_decode, &index),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !generation || index == UINT64_MAX) return MGMT_ERR_EINVAL;
	uint8_t data[256];
	struct bp_record_info info;
	int length = application_record_parameters_read(generation, index, data, &info, &revision);
	if (length < 0) return result(stream, length);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = number(zs, "generation", info.generation) && number(zs, "revision", revision) &&
		zcbor_tstr_put_lit(zs, "first") && zcbor_uint64_put(zs, info.first) &&
		zcbor_tstr_put_lit(zs, "next") && zcbor_uint64_put(zs, info.next) &&
		zcbor_tstr_put_lit(zs, "data") && zcbor_bstr_encode_ptr(zs, data, length);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int record_clear_write(struct smp_streamer *stream)
{
	return result(stream, application_clear_recording());
}

static int record_config_read(struct smp_streamer *stream)
{
	uint32_t generation = 0, offset = UINT32_MAX;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("generation", zcbor_uint32_decode, &generation),
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &offset),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !generation || offset == UINT32_MAX) return MGMT_ERR_EINVAL;
	uint8_t data[256];
	struct bp_record_info info;
	int length = application_record_config_read(generation, offset, data, sizeof(data), &info);
	if (length < 0) return result(stream, length);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = number(zs, "generation", info.generation) && number(zs, "total", info.config_size) &&
		zcbor_tstr_put_lit(zs, "data") && zcbor_bstr_encode_ptr(zs, data, length);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int record_trigger_write(struct smp_streamer *stream)
{
	uint32_t generation = 0, after = UINT32_MAX;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("generation", zcbor_uint32_decode, &generation),
		ZCBOR_MAP_DECODE_KEY_DECODER("after", zcbor_uint32_decode, &after),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !generation || after == UINT32_MAX) return MGMT_ERR_EINVAL;
	return result(stream, application_record_trigger(generation, after));
}

#ifdef CONFIG_BRICKPHYR_PYTHON
static int output_write(struct smp_streamer *stream)
{
	bool attached;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("attached", zcbor_bool_decode, &attached),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !zcbor_map_decode_bulk_key_found(fields, ARRAY_SIZE(fields), "attached")) {
		return MGMT_ERR_EINVAL;
	}
	application_output_stream(attached);
	return MGMT_ERR_EOK;
}

static int input_write(struct smp_streamer *stream)
{
	struct zcbor_string data = {0};
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("data", zcbor_bstr_decode, &data),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !data.value) {
		return MGMT_ERR_EINVAL;
	}
	return result(stream, application_input(data.value, data.len));
}

static int interrupt_write(struct smp_streamer *stream)
{
	return result(stream, application_interrupt());
}

static int repl_write(struct smp_streamer *stream)
{
	return result(stream, application_repl());
}
#endif

#ifdef CONFIG_BRICKPHYR_APP_STORAGE
static int file_arguments(struct smp_streamer *stream, char *name, size_t capacity,
			  uint32_t *length, uint32_t *stack)
{
	struct zcbor_string text = {0};
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("name", zcbor_tstr_decode, &text),
		ZCBOR_MAP_DECODE_KEY_DECODER("size", zcbor_uint32_decode, length),
		ZCBOR_MAP_DECODE_KEY_DECODER("stack", zcbor_uint32_decode, stack),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !text.value || text.len >= capacity || memchr(text.value, 0, text.len)) {
		return -EINVAL;
	}
	memcpy(name, text.value, text.len);
	name[text.len] = 0;
	return 0;
}

static int list_read(struct smp_streamer *stream)
{
	char after[APP_NAME_MAX + 1];
	uint32_t ignored = 0, stack = 0;
	int err = file_arguments(stream, after, sizeof(after), &ignored, &stack);
	struct app_file entry;
	size_t available;
	if (!err) {
		err = app_store_next(after, &entry, &available);
	}
	if (err) {
		return result(stream, err);
	}
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = zcbor_tstr_put_lit(zs, "name") &&
		zcbor_tstr_encode_ptr(zs, entry.name, strlen(entry.name)) &&
		number(zs, "size", entry.size) && number(zs, "available", available);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int file_operation(struct smp_streamer *stream, unsigned int command)
{
	char name[APP_NAME_MAX + 1];
	uint32_t length = UINT32_MAX, stack = 0;
	int err = file_arguments(stream, name, sizeof(name), &length, &stack);
	if (!err) {
		if (command == 7) {
			/* All file uploads and publication run serially on SMP's worker.
			 * Close also cancels a partial standard upload before validation. */
			err = fs_mgmt_file_close();
			if (!err) {
				err = app_store_publish(name, length);
			}
		} else {
			err = command == 8 ? app_store_remove(name) : app_store_run(name, stack);
		}
	}
	return result(stream, err);
}
static int publish_write(struct smp_streamer *s) { return file_operation(s, 7); }
static int remove_write(struct smp_streamer *s) { return file_operation(s, 8); }
static int stored_run_write(struct smp_streamer *s) { return file_operation(s, 9); }
#endif

static const struct mgmt_handler handlers[] = {
	[0] = {.mh_read = status_read},
	[1] = {.mh_write = upload_write},
	[2] = {.mh_write = run_write},
	[3] = {.mh_write = stop_write},
	[4] = {.mh_read = output_read,
#ifdef CONFIG_BRICKPHYR_PYTHON
		.mh_write = output_write,
#endif
	},
	[5] = {.mh_write = discard_write},
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
	[6] = {.mh_read = list_read},
	[7] = {.mh_write = publish_write},
	[8] = {.mh_write = remove_write},
	[9] = {.mh_write = stored_run_write},
#endif
#ifdef CONFIG_BRICKPHYR_PYTHON
	[10] = {.mh_write = input_write},
	[11] = {.mh_write = interrupt_write},
	[12] = {.mh_write = repl_write},
#endif
	[13] = {.mh_read = stream_read},
	[14] = {.mh_read = record_info_read, .mh_write = record_clear_write},
	[15] = {.mh_read = record_read},
	[16] = {.mh_read = record_config_read, .mh_write = record_trigger_write},
	[17] = {.mh_read = record_parameters_read},
	[18] = {.mh_read = memory_read},
	[19] = {.mh_read = stack_read},
	[20] = {.mh_read = budget_read},
	[21] = {.mh_read = record_budget_read},
	[22] = {.mh_read = record_block_read},
	[23] = {.mh_write = record_bind},
};

#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
static int legacy_error(uint16_t err)
{
	switch (err) {
	case ENOMEM: return MGMT_ERR_ENOMEM;
	case EINVAL: return MGMT_ERR_EINVAL;
	case ENODATA: return MGMT_ERR_ENOENT;
	case ENOTSUP: return MGMT_ERR_ENOTSUP;
	case EFBIG: return MGMT_ERR_EMSGSIZE;
	default: return MGMT_ERR_EUNKNOWN;
	}
}
#endif

static struct mgmt_group application_group = {
	.mg_handlers = handlers,
	.mg_handlers_count = ARRAY_SIZE(handlers),
	.mg_group_id = APPLICATION_GROUP,
#ifdef CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL
	.mg_translate_error = legacy_error,
#endif
};

static void setup(void)
{
	mgmt_register_group(&application_group);
}

MCUMGR_HANDLER_DEFINE(native_application, setup);
