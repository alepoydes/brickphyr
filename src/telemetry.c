// SPDX-License-Identifier: Apache-2.0
#include "hub_power.h"
#include "hub_matrix.h"
#include "hub_imu.h"
#include "hub_sound.h"
#include "hub_battery.h"
#include "hub_ports.h"
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
enum source { POWER, CENTER, SIDE, APPLICATION, MATRIX, IMU, IMU_CALIBRATED, IMU_ORIENTATION,
	SOUND, BATTERY, PORT_A, PORT_B, PORT_C, PORT_D, PORT_E, PORT_F,
	SENSOR_A, SENSOR_B, SENSOR_C, SENSOR_D, SENSOR_E, SENSOR_F,
	MOTOR_A, MOTOR_B, MOTOR_C, MOTOR_D, MOTOR_E, MOTOR_F };
static const char *const names[] = {"power", "buttons.center", "buttons.side", "application", "matrix",
	"imu", "imu.calibrated", "imu.orientation", "sound", "battery",
	"port.A", "port.B", "port.C", "port.D", "port.E", "port.F",
	"sensor.A", "sensor.B", "sensor.C", "sensor.D", "sensor.E", "sensor.F",
	"motor.A", "motor.B", "motor.C", "motor.D", "motor.E", "motor.F"};

/* A bounded presentation of current numeric observations. Device state and
 * behavior remain in their owning services. Shell and SMP share this capture,
 * including field names, units and validity; neither starts device I/O. */
struct observation {
	int64_t read_ms;
	int error;
	size_t count;
	struct { const char *name; int64_t value; } fields[18];
	bool frame;
	struct bp_matrix_state matrix;
	bool port_data;
	uint8_t data[32], data_length;
};

static bool available(enum source source)
{
	switch (source) {
	case POWER: return true;
	case CENTER: return DT_NODE_HAS_STATUS(DT_NODELABEL(center_keys), okay);
	case SIDE: return DT_NODE_HAS_STATUS(DT_NODELABEL(arrow_keys), okay);
	case APPLICATION: return IS_ENABLED(CONFIG_BRICKPHYR_NATIVE);
	case MATRIX: return IS_ENABLED(CONFIG_BRICKPHYR_MATRIX);
	case SOUND: return IS_ENABLED(CONFIG_BRICKPHYR_SOUND);
	case BATTERY: return IS_ENABLED(CONFIG_BRICKPHYR_BATTERY);
	case PORT_A: case PORT_B: case PORT_C: case PORT_D: case PORT_E: case PORT_F:
		return IS_ENABLED(CONFIG_BRICKPHYR_PORTS);
	case SENSOR_A: case SENSOR_B: case SENSOR_C: case SENSOR_D: case SENSOR_E: case SENSOR_F:
		return IS_ENABLED(CONFIG_BRICKPHYR_PORT_UART);
	case MOTOR_A: case MOTOR_B: case MOTOR_C: case MOTOR_D: case MOTOR_E: case MOTOR_F:
		return IS_ENABLED(CONFIG_BRICKPHYR_MOTORS);
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
	} else if (source >= MOTOR_A && source <= MOTOR_F) {
		struct bp_motor_state value;
		out->error = hub_motor_get(source - MOTOR_A, &value);
		if (!out->error) {
			out->error = value.error;
			field(out, "requested_ms", value.requested_ms);
			field(out, "applied_ms", value.applied_ms);
			field(out, "deadline_ms", value.deadline_ms);
			field(out, "generation", value.generation);
			field(out, "request", value.request);
			field(out, "completed", value.completed);
			field(out, "mode", value.mode);
			field(out, "duty_permille", value.duty);
			field(out, "requested_mode", value.requested_mode);
			field(out, "requested_duty", value.requested_duty);
			field(out, "owner", value.owner);
			field(out, "pending", value.pending);
			field(out, "releasing", value.releasing);
			field(out, "valid", value.valid);
			field(out, "cleanup", value.cleanup_error);
			field(out, "failures", value.failures);
		}
	} else if (source >= SENSOR_A && source <= SENSOR_F) {
		struct bp_sensor_sample value;
		out->error = hub_sensor_get(source - SENSOR_A, &value);
		if (!out->error) {
			out->error = value.error;
			field(out, "sample_ms", value.sample_ms);
			field(out, "sequence", value.sequence);
			field(out, "generation", value.generation);
			field(out, "type_id", value.type_id);
			field(out, "mode", value.mode);
			field(out, "measurement", value.measurement);
			field(out, "available", value.available);
			field(out, "owner", value.owner);
			field(out, "fields", value.fields);
			if (value.fields & BP_SENSOR_HAS_DISTANCE) field(out, "distance_mm", value.distance_mm);
			if (value.fields & BP_SENSOR_HAS_RELATIVE_DISTANCE) field(out, "distance_pct", value.distance_pct);
			if (value.fields & BP_SENSOR_HAS_REFLECTION) field(out, "reflection_permille", value.reflection_permille);
			if (value.fields & BP_SENSOR_HAS_AMBIENT) field(out, "ambient_permille", value.ambient_permille);
			if (value.fields & BP_SENSOR_HAS_COUNT) field(out, "count", value.count);
			if (value.fields & BP_SENSOR_HAS_PRESENCE) field(out, "presence", value.presence);
			if (value.fields & BP_SENSOR_HAS_ANGLE) field(out, "angle_mdeg", value.angle_mdeg);
			if (value.fields & BP_SENSOR_HAS_ABSOLUTE_ANGLE) field(out, "absolute_mdeg", value.absolute_mdeg);
			if (value.fields & BP_SENSOR_HAS_HSV) {
				field(out, "hue_deg", value.hue_deg);
				field(out, "saturation_pct", value.saturation_pct);
				field(out, "value_pct", value.value_pct);
			}
			if (value.fields & BP_SENSOR_HAS_FORCE) {
				field(out, "force_mn", value.force_mn);
				field(out, "travel_um", value.travel_um);
			}
			if (value.fields & BP_SENSOR_HAS_TILT) {
				field(out, "tilt0_deg", value.tilt_deg[0]);
				field(out, "tilt1_deg", value.tilt_deg[1]);
			}
		}
	} else if (source >= PORT_A && source <= PORT_F) {
		struct bp_port_state value;
		out->error = hub_port_get(source - PORT_A, &value);
		if (!out->error) {
			out->error = value.error;
			field(out, "sample_ms", value.sample_ms);
			field(out, "changed_ms", value.changed_ms);
			field(out, "sequence", value.sequence);
			field(out, "generation", value.generation);
			field(out, "failures", value.failures);
			field(out, "phase", value.phase);
			field(out, "type_id", value.type_id);
			field(out, "cleanup", value.cleanup_error);
			field(out, "supply", value.supply);
			if (value.phase == BP_PORT_NEGOTIATING || value.phase == BP_PORT_READY) {
				field(out, "data_ms", value.data_ms);
				field(out, "data_sequence", value.data_sequence);
				field(out, "mode", value.mode);
				field(out, "num_modes", value.num_modes);
				field(out, "data_type", value.data_type);
				field(out, "num_values", value.num_values);
				field(out, "baudrate", value.baudrate);
				field(out, "capabilities", value.capabilities);
				field(out, "bad_frames", value.bad_frames);
				out->port_data = true;
				out->data_length = value.length;
				memcpy(out->data, value.data, value.length);
			} else {
				field(out, "pressed", value.pressed);
			}
		}
	} else if (source == BATTERY) {
		struct bp_battery_sample value;
		out->error = hub_battery_get(&value);
		if (!out->error) {
			out->error = value.error;
			field(out, "sample_ms", value.sample_ms);
			field(out, "completed_ms", value.read_ms);
			field(out, "sequence", value.sequence);
			field(out, "failures", value.failures);
			field(out, "voltage_mv", value.voltage_mv);
			field(out, "current_ma", value.current_ma);
			field(out, "voltage_raw", value.voltage_raw);
			field(out, "current_raw", value.current_raw);
		}
	} else if (source == SOUND) {
		struct bp_sound_state value;
		out->error = hub_sound_get(&value);
		if (!out->error) {
			out->error = value.error;
			field(out, "changed_ms", value.changed_ms);
			field(out, "owner", value.owner);
			field(out, "phase", value.phase);
			field(out, "mode", value.mode);
			field(out, "requested_hz", value.requested_rate);
			field(out, "actual_hz", value.actual_rate);
			field(out, "queued", value.queued_samples);
			field(out, "completed", value.completed_samples);
			field(out, "frequency", value.frequency);
			field(out, "duration_ms", value.duration_ms);
			field(out, "volume", value.volume);
			field(out, "eof", value.eof);
			field(out, "underruns", value.underruns);
			field(out, "failures", value.failures);
			field(out, "cleanup", value.cleanup_error);
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
		text(zs, "values") && zcbor_map_start_encode(zs, ARRAY_SIZE(value.fields) + 2);
	for (size_t i = 0; ok && i < value.count; i++) {
		ok = number(zs, value.fields[i].name, value.fields[i].value);
	}
	if (ok && value.frame) {
		ok = text(zs, "requested") && zcbor_bstr_encode_ptr(zs, value.matrix.requested, BP_MATRIX_PIXELS) &&
			text(zs, "applied") && zcbor_bstr_encode_ptr(zs, value.matrix.applied, BP_MATRIX_PIXELS);
	}
	if (ok && value.port_data) {
		ok = text(zs, "data") && zcbor_bstr_encode_ptr(zs, value.data, value.data_length);
	}
	return ok && zcbor_map_end_encode(zs, ARRAY_SIZE(value.fields) + 2) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
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
		if (value.port_data) {
			shell_hexdump(shell, value.data, value.data_length);
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

SHELL_CMD_ARG_REGISTER(observe, NULL, "Read snapshots: [all | source]. Omit source to read all available observations.",
		      cmd_observe, 1, 1);

static int cmd_sensor(const struct shell *shell, size_t argc, char **argv)
{
	if (strlen(argv[1]) != 1 || argv[1][0] < 'A' || argv[1][0] > 'F') {
		return -EINVAL;
	}
	uint32_t port = argv[1][0] - 'A';
	if (!strcmp(argv[2], "release")) {
		return hub_port_release(BP_PORT_TERMINAL, port);
	}
	static const char *const modes[] = {"default", "surface", "ambient", "distance",
		"presence", "force", "tilt", "count", "angle"};
	BUILD_ASSERT(ARRAY_SIZE(modes) == BP_SENSOR_MEASUREMENT_COUNT);
	for (uint32_t i = 0; i < ARRAY_SIZE(modes); i++) {
		if (!strcmp(argv[2], modes[i])) {
			int err = hub_sensor_configure(BP_PORT_TERMINAL, port, i);
			shell_print(shell, "Sensor %c: %d; read observe sensor.%c", argv[1][0], err, argv[1][0]);
			return err;
		}
	}
	return -EINVAL;
}
SHELL_CMD_ARG_REGISTER(sensor, NULL,
	"Select measurement: A..F default|surface|ambient|distance|presence|force|tilt|count|angle|release.",
	cmd_sensor, 3, 0);

static int cmd_motor(const struct shell *shell, size_t argc, char **argv)
{
	if (strlen(argv[1]) != 1 || argv[1][0] < 'A' || argv[1][0] > 'F') return -EINVAL;
	uint32_t port = argv[1][0] - 'A';
	if (argc == 3 && !strcmp(argv[2], "release")) return hub_port_release(BP_PORT_TERMINAL, port);
	if (argc == 3 && !strcmp(argv[2], "coast")) return hub_motor_coast(BP_PORT_TERMINAL, port);
	bool brake = !strcmp(argv[2], "brake");
	if ((!brake && (strcmp(argv[2], "duty") || argc < 4)) || argc > (brake ? 4 : 5)) return -EINVAL;
	int err = 0;
	long duty = brake ? 0 : shell_strtol(argv[3], 10, &err);
	if (err || duty < -1000 || duty > 1000) return -EINVAL;
	size_t duration_index = brake ? 3 : 4;
	unsigned long duration = argc > duration_index ? shell_strtoul(argv[duration_index], 10, &err) : 0;
	if (err || duration > INT32_MAX || (argc > duration_index && argv[duration_index][0] == '-')) return -EINVAL;
	return hub_motor_drive(BP_PORT_TERMINAL, port, duty, duration);
}
SHELL_CMD_ARG_REGISTER(motor, NULL, "A..F duty PERMILLE [MS] | brake [MS] | coast | release.",
	cmd_motor, 3, 2);

static const struct mgmt_handler handlers[] = {
	[0] = {.mh_read = info_read}, [1] = {.mh_read = snapshot_read},
};
static struct mgmt_group group = {
	.mg_group_id = TELEMETRY_GROUP, .mg_handlers = handlers,
	.mg_handlers_count = ARRAY_SIZE(handlers),
};
static void setup(void) { mgmt_register_group(&group); }
MCUMGR_HANDLER_DEFINE(telemetry, setup);
