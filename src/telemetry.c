// SPDX-License-Identifier: Apache-2.0
#include "hub_power.h"
#include "hub_matrix.h"
#include "hub_imu.h"
#include "input_resistor_ladder.h"
#include "native.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/shell/shell.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#if __has_include(<zephyr/app_version.h>)
#include <zephyr/app_version.h>
#else
#define APP_VERSION_STRING "unversioned"
#define APP_BUILD_VERSION unversioned
#endif

#define TELEMETRY_GROUP 66
enum source { POWER, CENTER, SIDE, APPLICATION, MATRIX, IMU, IMU_CALIBRATED, IMU_ORIENTATION };
static const char *const names[] = {"power", "buttons.center", "buttons.side", "application", "matrix",
	"imu", "imu.calibrated", "imu.orientation"};

/* A bounded presentation of current numeric observations. Device state and
 * behavior remain in their owning services. Shell and SMP share this capture,
 * including field names, units and validity; neither starts device I/O. */
struct observation {
	int64_t read_ms;
	int error;
	size_t count;
	struct { const char *name; int64_t value; } fields[16];
	bool frame;
	struct bp_matrix_state matrix;
};

static bool available(enum source source)
{
	switch (source) {
	case POWER: return true;
	case CENTER: return DT_NODE_HAS_STATUS(DT_NODELABEL(center_keys), okay);
	case SIDE: return DT_NODE_HAS_STATUS(DT_NODELABEL(arrow_keys), okay);
	case APPLICATION: return IS_ENABLED(CONFIG_BRICKPHYR_NATIVE);
	case MATRIX: return IS_ENABLED(CONFIG_BRICKPHYR_MATRIX);
	case IMU: case IMU_CALIBRATED: case IMU_ORIENTATION: return IS_ENABLED(CONFIG_BRICKPHYR_IMU);
	default: return false;
	}
}

static int find_source(const char *name, size_t length)
{
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		if (strlen(names[i]) == length && !memcmp(names[i], name, length)) {
			return i;
		}
	}
	return -ENOENT;
}

static void field(struct observation *out, const char *name, int64_t value)
{
	__ASSERT_NO_MSG(out->count < ARRAY_SIZE(out->fields));
	out->fields[out->count].name = name;
	out->fields[out->count++].value = value;
}

static void capture(enum source source, struct observation *out)
{
	*out = (struct observation){0};
	if (!available(source)) {
		out->error = -ENOTSUP;
	} else if (source == POWER) {
		struct hub_power_state value;
		hub_power_get(&value);
		out->error = value.source < 0 ? value.source : 0;
		field(out, "sample_ms", value.sampled_ms);
		field(out, "source", value.source);
		field(out, "idle_since_ms", value.idle_since_ms);
		field(out, "battery_idle_s", value.battery_idle_s);
		field(out, "usb_idle_s", value.usb_idle_s);
		field(out, "connected", value.connected);
		field(out, "application", value.application);
		field(out, "shutdown", value.shutting_down);
	} else if (source == CENTER || source == SIDE) {
		const struct device *dev = source == CENTER ?
			DEVICE_DT_GET_OR_NULL(DT_NODELABEL(center_keys)) :
			DEVICE_DT_GET_OR_NULL(DT_NODELABEL(arrow_keys));
		if (!device_is_ready(dev)) {
			out->error = -ENODEV;
		} else {
#if DT_HAS_COMPAT_STATUS_OKAY(lego_resistor_ladder)
			struct resistor_ladder_sample value = resistor_ladder_get_sample(dev);
			out->error = value.error ? value.error : value.stable ? 0 : -EAGAIN;
			field(out, "sample_ms", value.sampled_ms);
			field(out, "raw", value.raw);
			field(out, "failures", value.failures);
			field(out, "stable", value.stable);
			if (source == CENTER) {
				field(out, "center", resistor_ladder_sample_key(dev, &value, INPUT_KEY_ENTER));
			} else {
				field(out, "left", resistor_ladder_sample_key(dev, &value, INPUT_KEY_LEFT));
				field(out, "right", resistor_ladder_sample_key(dev, &value, INPUT_KEY_RIGHT));
				field(out, "bluetooth", resistor_ladder_sample_key(dev, &value, INPUT_KEY_BLUETOOTH));
			}
#endif
		}
	} else if (source == APPLICATION) {
#ifdef CONFIG_BRICKPHYR_NATIVE
		struct native_status value;
		out->error = native_observe(&value);
		if (!out->error) {
			field(out, "state", value.state);
			field(out, "result", value.result);
			field(out, "fault", value.fault);
			field(out, "cleanup", value.cleanup_error);
			field(out, "image", value.image_bytes);
			field(out, "heap", value.loader_bytes);
			field(out, "loaded", value.loaded_bytes);
			field(out, "stack", value.stack_bytes);
			field(out, "unused", value.stack_unused);
			field(out, "dropped", value.output_dropped);
			field(out, "queued", value.output_bytes);
			field(out, "received", value.upload_bytes);
			field(out, "total", value.upload_size);
			field(out, "ready", value.upload_ready);
		}
#endif
	} else if (source == IMU) {
		struct bp_imu_sample value;
		out->error = hub_imu_get(&value);
		if (!out->error) {
			out->error = value.error;
			field(out, "sample_us", value.sample_us);
			field(out, "completed_us", value.read_us);
			field(out, "sequence", value.sequence);
			field(out, "missed", value.missed);
			field(out, "failures", value.failures);
			field(out, "ax_um_s2", value.acceleration[0]);
			field(out, "ay_um_s2", value.acceleration[1]);
			field(out, "az_um_s2", value.acceleration[2]);
			field(out, "gx_mdps", value.angular_velocity[0]);
			field(out, "gy_mdps", value.angular_velocity[1]);
			field(out, "gz_mdps", value.angular_velocity[2]);
		}
	} else if (source == IMU_CALIBRATED || source == IMU_ORIENTATION) {
		struct bp_imu_state value;
		out->error = hub_imu_get_state(&value);
		if (!out->error) {
			out->error = value.error;
			field(out, "sample_us", value.raw.sample_us);
			field(out, "sequence", value.raw.sequence);
			field(out, "epoch", value.epoch);
			field(out, "ready", value.ready);
			if (source == IMU_CALIBRATED) {
				field(out, "stationary", value.stationary);
				field(out, "ax_um_s2", value.acceleration[0]);
				field(out, "ay_um_s2", value.acceleration[1]);
				field(out, "az_um_s2", value.acceleration[2]);
				field(out, "gx_mdps", value.angular_velocity[0]);
				field(out, "gy_mdps", value.angular_velocity[1]);
				field(out, "gz_mdps", value.angular_velocity[2]);
				field(out, "bx_mdps", value.bias[0]);
				field(out, "by_mdps", value.bias[1]);
				field(out, "bz_mdps", value.bias[2]);
			} else {
				static const char *const elements[] = {
					"m11", "m12", "m13", "m21", "m22", "m23", "m31", "m32", "m33",
				};
				for (size_t i = 0; i < ARRAY_SIZE(elements); i++) {
					field(out, elements[i], lroundf(value.orientation[i] * 1000000));
				}
				field(out, "heading1_mdeg", llround((double)value.heading_1d * 1000));
				field(out, "heading3_mdeg", llround((double)value.heading_3d * 1000));
			}
		}
	} else if (source == MATRIX) {
		out->error = hub_matrix_get(&out->matrix);
		if (!out->error) {
			struct bp_matrix_state *value = &out->matrix;
			out->frame = true;
			out->error = value->error ? value->error : value->valid ? 0 : -ENODATA;
			field(out, "applied_ms", value->applied_ms);
			field(out, "failures", value->errors);
			field(out, "owner", value->owner);
			field(out, "pending", value->pending);
			field(out, "valid", value->valid);
		}
	}
	out->read_ms = k_uptime_get();
}

static bool text(zcbor_state_t *zs, const char *value)
{
	return zcbor_tstr_encode_ptr(zs, value, strlen(value));
}

static bool number(zcbor_state_t *zs, const char *name, int64_t value)
{
	return text(zs, name) && zcbor_int64_put(zs, value);
}

static int info_read(struct smp_streamer *stream)
{
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = number(zs, "api", 1) && text(zs, "firmware") && text(zs, APP_VERSION_STRING) &&
		text(zs, "build") && text(zs, STRINGIFY(APP_BUILD_VERSION)) &&
		text(zs, "board") && text(zs, CONFIG_BOARD_TARGET) &&
		text(zs, "sources") && zcbor_list_start_encode(zs, ARRAY_SIZE(names));
	for (size_t i = 0; ok && i < ARRAY_SIZE(names); i++) {
		if (available(i)) {
			ok = text(zs, names[i]);
		}
	}
	return ok && zcbor_list_end_encode(zs, ARRAY_SIZE(names)) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int snapshot_read(struct smp_streamer *stream)
{
	struct zcbor_string source;
	size_t decoded;
	struct zcbor_map_decode_key_val fields[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("source", zcbor_tstr_decode, &source),
	};
	if (zcbor_map_decode_bulk(stream->reader->zs, fields, ARRAY_SIZE(fields), &decoded) ||
	    decoded != 1) {
		return MGMT_ERR_EINVAL;
	}
	int selected = find_source((const char *)source.value, source.len);
	if (selected < 0) {
		return MGMT_ERR_ENOENT;
	}
	struct observation value;
	capture(selected, &value);
	zcbor_state_t *zs = stream->writer->zs;
	bool ok = number(zs, "api", 1) && text(zs, "source") && text(zs, names[selected]) &&
		number(zs, "read_ms", value.read_ms) && number(zs, "error", value.error) &&
		text(zs, "values") && zcbor_map_start_encode(zs, ARRAY_SIZE(value.fields));
	for (size_t i = 0; ok && i < value.count; i++) {
		ok = number(zs, value.fields[i].name, value.fields[i].value);
	}
	if (ok && value.frame) {
		ok = text(zs, "requested") && zcbor_bstr_encode_ptr(zs, value.matrix.requested, BP_MATRIX_PIXELS) &&
			text(zs, "applied") && zcbor_bstr_encode_ptr(zs, value.matrix.applied, BP_MATRIX_PIXELS);
	}
	return ok && zcbor_map_end_encode(zs, ARRAY_SIZE(value.fields)) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int cmd_observe(const struct shell *shell, size_t argc, char **argv)
{
	bool all = argc == 1 || !strcmp(argv[1], "all");
	int selected = all ? 0 : find_source(argv[1], strlen(argv[1]));
	if (selected < 0) {
		return selected;
	}
	int result = 0;
	for (size_t i = selected; i < (all ? ARRAY_SIZE(names) : selected + 1); i++) {
		if (all && !available(i)) {
			continue;
		}
		struct observation value;
		capture(i, &value);
		shell_print(shell, "%s read_ms=%" PRId64 " error=%d", names[i], value.read_ms, value.error);
		for (size_t j = 0; j < value.count; j++) {
			shell_print(shell, "  %s=%" PRId64, value.fields[j].name, value.fields[j].value);
		}
		if (value.frame) {
			shell_print(shell, "  requested / applied (row-major brightness 0..100):");
			for (size_t j = 0; j < BP_MATRIX_PIXELS; j += 5) {
				const uint8_t *r = value.matrix.requested + j, *a = value.matrix.applied + j;
				shell_print(shell, "  %3u %3u %3u %3u %3u / %3u %3u %3u %3u %3u",
					r[0], r[1], r[2], r[3], r[4], a[0], a[1], a[2], a[3], a[4]);
			}
		}
		if (!result) {
			result = value.error;
		}
	}
	return result;
}

SHELL_CMD_ARG_REGISTER(observe, NULL, "Read snapshots: [all | power | buttons.center | buttons.side | application | matrix | imu | imu.calibrated | imu.orientation].",
		      cmd_observe, 1, 1);

static const struct mgmt_handler handlers[] = {
	[0] = {.mh_read = info_read}, [1] = {.mh_read = snapshot_read},
};
static struct mgmt_group group = {
	.mg_group_id = TELEMETRY_GROUP, .mg_handlers = handlers,
	.mg_handlers_count = ARRAY_SIZE(handlers),
};
static void setup(void) { mgmt_register_group(&group); }
MCUMGR_HANDLER_DEFINE(telemetry, setup);
