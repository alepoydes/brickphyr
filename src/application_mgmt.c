// SPDX-License-Identifier: Apache-2.0
#define __ZEPHYR_SUPERVISOR__
#include "application.h"
#include "application_record.h"
#include "native_image.h"
#include "app_store.h"
#include "python_source.h"
#ifdef CONFIG_BRICKPHYR_CAPTURE
#include "capture.h"
#include "telemetry_block.h"
#include <brickphyr_capture.h>
#endif
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt.h>
#endif

#include <brickphyr_abi.h>
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
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
/* Record replies also carry a control step and, when they fit, applied parameters.
 * Keep enough room for worst-case position fields, CBOR framing and SMP header. */
#define RECORD_INLINE_PARAMETERS_MAX (CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - 208)

static bool record_inline_parameters(const struct bp_record_info *info, bool segments)
{
	return segments && info->control && info->parameter_size &&
		info->parameter_size <= RECORD_INLINE_PARAMETERS_MAX;
}
#ifdef CONFIG_BRICKPHYR_CAPTURE
/* Leave room for the position, row count and CBOR/SMP framing. */
#define RECORD_COMPACT_BYTES MIN(2048, CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - 144)
BUILD_ASSERT(RECORD_COMPACT_BYTES >= 104);

static bool record_compact_supported(const struct bp_record_info *info, bool variable,
	bool segments)
{
	if (segments && info->parameter_size && !record_inline_parameters(info, segments)) return false;
	size_t context = segments && info->parameter_size ? info->parameter_size + 16 : 0;
	bool standard = info->config_size == CONFIG_SIZE &&
		!strcmp(info->spec.schema, "brickphyr.capture/2");
	bool application = info->config_size > CONFIG_SIZE + 1 &&
		info->config_size <= BP_CAPTURE_APP_MAX_CONFIG_SIZE &&
		!strcmp(info->spec.schema, "brickphyr.capture+app/1");
	return info->control && !variable && (standard || application) &&
		context < RECORD_COMPACT_BYTES &&
		info->spec.size + 32 + 8 <= RECORD_COMPACT_BYTES - context;
}
#endif

static int record_info_read(struct smp_streamer *stream)
{
	struct bp_record_info info;
	uint8_t tag[APPLICATION_RECORD_TAG_SIZE];
	bool variable;
	bool paused, segments;
	application_record_snapshot(&info, tag, &variable, &paused, &segments);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = record_position(zs, &info) && zcbor_tstr_put_lit(zs, "tag") &&
		zcbor_bstr_encode_ptr(zs, tag, sizeof(tag)) && number(zs, "block", RECORD_BLOCK_BYTES) &&
		number(zs, "size", info.spec.size) &&
		number(zs, "config_size", info.config_size) &&
		zcbor_tstr_put_lit(zs, "control") && zcbor_bool_put(zs, info.control) &&
		zcbor_tstr_put_lit(zs, "variable") && zcbor_bool_put(zs, variable) &&
		zcbor_tstr_put_lit(zs, "paused") && zcbor_bool_put(zs, paused) &&
		zcbor_tstr_put_lit(zs, "segments") && zcbor_bool_put(zs, segments) &&
		zcbor_tstr_put_lit(zs, "inline_parameters") &&
		zcbor_bool_put(zs, record_inline_parameters(&info, segments)) &&
#ifdef CONFIG_BRICKPHYR_CAPTURE
		zcbor_tstr_put_lit(zs, "compact") &&
		zcbor_bool_put(zs, record_compact_supported(&info, variable, segments)) &&
		zcbor_tstr_put_lit(zs, "compact_ack") &&
		zcbor_bool_put(zs, paused && record_compact_supported(&info, variable, segments)) &&
#endif
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
	bool segments;
	struct bp_record_info current;
	application_record_snapshot(&current, NULL, NULL, NULL, &segments);
	if (current.generation != generation) return result(stream, -ESTALE);
	if (segments && index > info.first && index < info.next) return result(stream, -EINVAL);
	/* The step and inline parameters replace payload room within the SMP budget. */
	if (info.control) {
		size_t capacity = 224;
		if (record_inline_parameters(&info, segments)) {
			capacity = MIN(capacity,
				CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - 176 - info.parameter_size);
		}
		length = MIN(length, capacity);
	}
	uint8_t parameters[256];
	uint32_t revision = 0;
	bool inline_parameters = record_inline_parameters(&info, segments);
	if (inline_parameters && length) {
		struct bp_record_info param_info;
		int size = application_record_parameters_read(generation, index, parameters,
			&param_info, &revision);
		if (size != info.parameter_size || param_info.generation != generation ||
			revision != step.parameter_revision)
			return result(stream, size < 0 ? size : -ESTALE);
	}
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = record_position(zs, &info) && zcbor_tstr_put_lit(zs, "data") &&
		zcbor_bstr_encode_ptr(zs, data, length) && zcbor_tstr_put_lit(zs, "step") &&
		zcbor_bstr_encode_ptr(zs, (void *)&step, info.control && length ? sizeof(step) : 0);
	if (ok && inline_parameters && length) {
		ok = zcbor_tstr_put_lit(zs, "parameters") &&
			zcbor_bstr_encode_ptr(zs, parameters, info.parameter_size);
	}
	if (!ok) return MGMT_ERR_EMSGSIZE;
	if (!length || offset + length != info.spec.size) return MGMT_ERR_EOK;
	int err = application_record_advance(generation, index, index + 1);
	return err ? result(stream, err) : MGMT_ERR_EOK;
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
	bool segments;
	struct bp_record_info current;
	application_record_snapshot(&current, NULL, NULL, NULL, &segments);
	if (current.generation != generation) return result(stream, -ESTALE);
	if (segments && index > info.first && index < info.next) return result(stream, -EINVAL);
	if (segments && info.control && info.parameter_size) return result(stream, -ENOTSUP);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = record_position(zs, &info) && zcbor_tstr_put_lit(zs, "data") &&
		zcbor_bstr_encode_ptr(zs, data, length);
	if (!ok) return MGMT_ERR_EMSGSIZE;
	size_t stride = info.spec.size + (info.control ? sizeof(struct bp_control_step) : 0);
	uint64_t completed = length && stride ? (offset + length) / stride : 0;
	if (!completed) return MGMT_ERR_EOK;
	int err = application_record_advance(generation, index, index + completed);
	return err ? result(stream, err) : MGMT_ERR_EOK;
}

#ifdef CONFIG_BRICKPHYR_CAPTURE
static int record_compact_empty(struct smp_streamer *stream, const struct bp_record_info *info)
{
	uint8_t empty = 0;
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = record_position(zs, info) && number(zs, "rows", 0) &&
		zcbor_tstr_put_lit(zs, "data") && zcbor_bstr_encode_ptr(zs, &empty, 0);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/* Compress retained rows in the management context, never in the controller's
 * release path. Each response starts with a complete control step and frame. */
static int record_compact_read(struct smp_streamer *stream)
{
	uint32_t generation = 0;
	uint32_t format = 0;
	uint64_t index = UINT64_MAX;
	uint64_t ack = 0;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("generation", zcbor_uint32_decode, &generation),
		ZCBOR_MAP_DECODE_KEY_DECODER("format", zcbor_uint32_decode, &format),
		ZCBOR_MAP_DECODE_KEY_DECODER("index", zcbor_uint64_decode, &index),
		ZCBOR_MAP_DECODE_KEY_DECODER("ack", zcbor_uint64_decode, &ack),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !generation || index == UINT64_MAX || (ack && ack != index) ||
	    (format && format != 4)) return MGMT_ERR_EINVAL;
	struct bp_record_info info;
	bool variable, paused, segments;
	application_record_snapshot(&info, NULL, &variable, &paused, &segments);
	if (info.generation != generation) return result(stream, -ESTALE);
	if (!record_compact_supported(&info, variable, segments)) return result(stream, -ENOTSUP);
	if (ack && !paused) return result(stream, -ENOTSUP);
	int err;
	/* The host flushed this prefix before asking for the next block. */
	if (ack) {
		err = application_record_ack(generation, ack, application_session_id());
		if (err) return result(stream, err);
	}
	application_record_snapshot(&info, NULL, NULL, NULL, NULL);
	if (index < info.first || index >= info.next) {
		return record_compact_empty(stream, &info);
	}
	if (segments && index != info.first) return result(stream, -EINVAL);
	uint8_t parameters[256];
	uint32_t revision = 0;
	if (segments && info.parameter_size) {
		struct bp_record_info parameter_info;
		int size = application_record_parameters_read(generation, index, parameters,
			&parameter_info, &revision);
		if (size != info.parameter_size || parameter_info.generation != generation)
			return result(stream, size < 0 ? size : -ESTALE);
	}
	uint8_t config[BP_CAPTURE_APP_MAX_CONFIG_SIZE];
	struct bp_record_info current;
	int length = application_record_config_read(generation, 0, config,
		info.config_size, &current);
	if (length != info.config_size || current.generation != generation) {
		return result(stream, current.generation != generation ? -ESTALE : -EINVAL);
	}
	size_t row_size = info.spec.size + sizeof(struct bp_control_step);
	bool application = info.config_size != CONFIG_SIZE;
	int count = application ? capture_app_wire_layout(config, info.config_size,
		info.spec.size, NULL, 0) : capture_wire_layout(config, info.spec.size, NULL, 0);
	if (count < 0) return result(stream, count);
	struct telemetry_wire_field *wire = k_malloc((count + 6) * sizeof(*wire) +
		RECORD_COMPACT_BYTES + 2 * row_size);
	if (!wire) return result(stream, -ENOMEM);
	uint8_t *encoded = (uint8_t *)(wire + count + 6);
	uint8_t *rows = encoded + RECORD_COMPACT_BYTES;
	int filled = application ? capture_app_wire_layout(config, info.config_size,
		info.spec.size, wire + 6, count) :
		capture_wire_layout(config, info.spec.size, wire + 6, count);
	if (filled != count) {
		k_free(wire);
		return result(stream, filled < 0 ? filled : -EINVAL);
	}
	static const uint8_t step_widths[] = {8, 8, 4, 4, 4, 4};
	size_t offset = 0;
	for (unsigned int field = 0; field < ARRAY_SIZE(step_widths); field++) {
		wire[field] = (struct telemetry_wire_field){offset, step_widths[field], -1, 1};
		offset += step_widths[field];
	}
	for (int field = 6; field < count + 6; field++) {
		wire[field].offset += sizeof(struct bp_control_step);
		if (wire[field].alias >= 0) wire[field].alias += 6;
	}
	struct telemetry_block block;
	size_t context = segments && info.parameter_size ? info.parameter_size + 16 : 0;
	err = telemetry_block_init(&block, encoded, RECORD_COMPACT_BYTES - context,
		rows, row_size, wire, count + 6, format == 4);
	if (err) {
		k_free(wire);
		return result(stream, err);
	}
	while (index + block.rows < info.next) {
		struct bp_control_step step;
		uint64_t cursor = index + block.rows;
		uint8_t *row = rows + row_size;
		length = application_record_read(generation, cursor, 0,
			row + sizeof(step), info.spec.size, &current, &step);
		if (current.generation != generation || cursor < current.first ||
		    cursor >= current.next || length != info.spec.size) {
			err = current.generation != generation ? -ESTALE :
				cursor < current.first ? -EOVERFLOW : -EIO;
			break;
		}
		if (segments && info.parameter_size && step.parameter_revision != revision) {
			err = block.rows ? 0 : -ESTALE;
			break;
		}
		memcpy(row, &step, sizeof(step));
		err = telemetry_block_append(&block, row);
		if (err == -ENOSPC && block.rows) {
			err = 0;
			break;
		}
		if (err) break;
	}
	application_record_snapshot(&current, NULL, NULL, NULL, NULL);
	if (current.generation != generation) err = -ESTALE;
	else if (!paused && index < current.first) {
		/* The ring overtook this block while it was encoded. No partial
		 * block or stale parameter reference may reach the collector. */
		k_free(wire);
		return record_compact_empty(stream, &current);
	} else if (!err && index < current.first) err = -ESTALE;
	if (err) {
		k_free(wire);
		return result(stream, err);
	}
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = record_position(zs, &current) && number(zs, "rows", block.rows) &&
		zcbor_tstr_put_lit(zs, "data") &&
		zcbor_bstr_encode_ptr(zs, encoded, block.rows ? block.length : 0);
	if (ok && segments && info.parameter_size && block.rows) {
		ok = zcbor_tstr_put_lit(zs, "parameters") &&
			zcbor_bstr_encode_ptr(zs, parameters, info.parameter_size);
	}
	k_free(wire);
	if (!ok) return MGMT_ERR_EMSGSIZE;
	if (!block.rows) return MGMT_ERR_EOK;
	err = application_record_advance(generation, index, index + block.rows);
	return err ? result(stream, err) : MGMT_ERR_EOK;
}
#endif

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

static int record_ack_write(struct smp_streamer *stream)
{
	uint32_t generation = 0;
	uint64_t next = UINT64_MAX;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("generation", zcbor_uint32_decode, &generation),
		ZCBOR_MAP_DECODE_KEY_DECODER("next", zcbor_uint64_decode, &next),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    !generation || next == UINT64_MAX) return MGMT_ERR_EINVAL;
	return result(stream, application_record_ack(generation, next, application_session_id()));
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
	[24] = {.mh_write = record_ack_write},
#ifdef CONFIG_BRICKPHYR_CAPTURE
	[25] = {.mh_read = record_compact_read},
#endif
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
