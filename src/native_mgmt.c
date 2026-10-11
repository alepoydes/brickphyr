// SPDX-License-Identifier: Apache-2.0
#include "native.h"
#include "native_image.h"
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
#include "app_store.h"
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
BUILD_ASSERT(CONFIG_MCUMGR_SMP_CBOR_MAX_MAIN_MAP_ENTRIES >= 17);

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
	struct native_status status;
	native_get_status(&status);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = number(zs, "api", 1) &&
		zcbor_tstr_put_lit(zs, "abi") && zcbor_tstr_put_lit(zs, BRICKPHYR_ABI) &&
		number(zs, "state", status.state) && number(zs, "result", status.result) &&
		number(zs, "fault", status.fault) && number(zs, "cleanup", status.cleanup_error) &&
		number(zs, "image", status.image_bytes) && number(zs, "heap", status.loader_bytes) &&
		number(zs, "loaded", status.loaded_bytes) && number(zs, "stack", status.stack_bytes) &&
		number(zs, "unused", status.stack_unused) && number(zs, "dropped", status.output_dropped) &&
		number(zs, "queued", status.output_bytes) &&
		number(zs, "received", status.upload_bytes) && number(zs, "total", status.upload_size) &&
		zcbor_tstr_put_lit(zs, "ready") && zcbor_bool_put(zs, status.upload_ready) &&
		number(zs, "limit", NATIVE_IMAGE_LIMIT);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int upload_write(struct smp_streamer *stream)
{
	uint32_t offset = UINT32_MAX, total = 0;
	struct zcbor_string data = {0};
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &offset),
		ZCBOR_MAP_DECODE_KEY_DECODER("total", zcbor_uint32_decode, &total),
		ZCBOR_MAP_DECODE_KEY_DECODER("data", zcbor_bstr_decode, &data),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    decoded != ARRAY_SIZE(fields)) {
		return MGMT_ERR_EINVAL;
	}
	size_t next;
	int err = native_upload(offset, total, data.value, data.len, &next);
	if (err) {
		return result(stream, err);
	}
	return number(stream->writer->zs, "off", next) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int run_write(struct smp_streamer *stream)
{
	return result(stream, native_run_uploaded());
}

static int stop_write(struct smp_streamer *stream)
{
	return result(stream, native_stop());
}

static int output_read(struct smp_streamer *stream)
{
	uint8_t data[128];
	size_t length = native_output(data, sizeof(data));
	bool ok = zcbor_tstr_put_lit(stream->writer->zs, "data") &&
		zcbor_bstr_encode_ptr(stream->writer->zs, data, length);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int discard_write(struct smp_streamer *stream)
{
	native_discard_upload();
	return MGMT_ERR_EOK;
}

#ifdef CONFIG_BRICKPHYR_APP_STORAGE
static int file_arguments(struct smp_streamer *stream, char *name, size_t capacity,
			  uint32_t *length)
{
	struct zcbor_string text = {0};
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("name", zcbor_tstr_decode, &text),
		ZCBOR_MAP_DECODE_KEY_DECODER("size", zcbor_uint32_decode, length),
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
	uint32_t ignored = 0;
	int err = file_arguments(stream, after, sizeof(after), &ignored);
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
	uint32_t length = UINT32_MAX;
	int err = file_arguments(stream, name, sizeof(name), &length);
	if (!err) {
		if (command == 7) {
			/* All file uploads and publication run serially on SMP's worker.
			 * Close also cancels a partial standard upload before validation. */
			err = fs_mgmt_file_close();
			if (!err) {
				err = app_store_publish(name, length);
			}
		} else {
			err = command == 8 ? app_store_remove(name) : app_store_run(name);
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
	[4] = {.mh_read = output_read},
	[5] = {.mh_write = discard_write},
#ifdef CONFIG_BRICKPHYR_APP_STORAGE
	[6] = {.mh_read = list_read},
	[7] = {.mh_write = publish_write},
	[8] = {.mh_write = remove_write},
	[9] = {.mh_write = stored_run_write},
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
