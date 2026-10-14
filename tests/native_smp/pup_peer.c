// SPDX-License-Identifier: Apache-2.0
/* Synthetic LUMP and raw UART peer for protected Python port tests. Wire values
 * are chosen independently of the firmware's parser and conversion code. */
#include "../ports/wiring.h"

#include <errno.h>
#include <lego/lump.h>
#include <string.h>
#include <zephyr/drivers/pwm/pwm_fake.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

static const uint8_t samples[4][8] = {
	{0x80, 0x7f},
	{0x85, 0xff, 0xff, 0x7f},
	{0, 0, 0, 0x80, 0xff, 0xff, 0xff, 0x7f},
	{0, 0, 0xa0, 0x3f, 0, 0, 0, 0xbf},
};
static const uint8_t sample_lengths[] = {2, 4, 8, 8};
static const uint8_t force_formats[][2] = {
	{1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 1}, {1, 0}, {8, 1},
};
static const uint8_t infrared_formats[][2] = {{1, 0}, {1, 2}, {3, 1}};
static const uint8_t force_calibration[] = {
	0, 0, 50, 0, 100, 0, 0, 0, 0, 0, 0, 0, 0x4c, 0x04, 0, 0,
};
static const uint8_t force_raw[] = {0x58, 0x02}; /* 600 counts. */
static const uint8_t tilt_raw[] = {0xec, 0x1e}; /* -20, +30 degrees on wire. */
static const uint8_t infrared_cal[] = {100, 0, 0, 0, 0, 0};
static const uint8_t color_formats[][2] = {
	{1, 0}, {1, 0}, {1, 0}, {3, 0}, {2, 1}, {4, 1}, {3, 1}, {4, 1}, {2, 1}, {7, 1},
};
static const uint8_t ultrasonic_formats[][2] = {
	{1, 1}, {1, 1}, {1, 1}, {1, 0}, {1, 2}, {4, 0}, {1, 0}, {1, 1}, {7, 1},
};
static const uint8_t color_distance_formats[][2] = {
	{1, 0}, {1, 0}, {1, 2}, {1, 0}, {1, 0}, {1, 0}, {3, 1}, {1, 1},
	{4, 0}, {2, 1}, {8, 1},
};
static const uint8_t matrix_formats[][2] = {{1, 0}, {1, 0}, {9, 0}, {1, 0}};
/* Technic L motor (ID 46): POWER, SPEED, POS, APOS, CALIB, STATS. */
static const uint8_t motor_formats[][2] = {
	{1, 0}, {1, 0}, {1, 2}, {1, 1}, {2, 1}, {14, 0},
};
static const uint8_t color_rgb[] = {0, 4, 0, 0, 0, 0, 0, 0}; /* 1024, 0, 0, 0. */
static const uint8_t color_shsv[] = {120, 0, 0xe8, 3, 0xe8, 3, 0, 0};
static const uint8_t ultrasonic_distance[] = {0x41, 1}; /* 321 mm. */
static const uint8_t color_distance_rgb[] = {0x90, 1, 0, 0, 0, 0}; /* 400, 0, 0. */
enum peer_kind { GENERIC, FORCE, TILT, INFRARED, COLOR, ULTRASONIC, COLOR_DISTANCE, MATRIX,
	ENCODED_MOTOR };
enum peer_phase { WAIT_SPEED, SEND_METADATA, STREAM };
#define PF_HISTORY_CAPACITY 24
struct pf_record { int64_t ms; uint16_t message; };
struct peer {
	const struct device *uart;
	uint8_t port;
	struct k_work_delayable work;
	enum peer_kind kind;
	enum peer_phase phase;
	int16_t shaft_angle, shaft_step;
	uint8_t infrared_count[4];
	uint8_t written[4][8];
	bool has_written[4], extension_valid;
	uint8_t extension, extension_packet[3];
	uint8_t output_wire[37], output_mode, output_length;
	bool has_output;
	struct pf_record pf_history[PF_HISTORY_CAPACITY];
	uint8_t pf_count;
	uint32_t pf_dropped;
	bool installed, connected, serial_mode, hold_mode, hold_reset;
	uint8_t command[34], used, needed;
	int selected;
	uint8_t metadata_mode;
	bool metadata_header_sent;
};
static struct peer peers[] = {
	{.uart = DEVICE_DT_GET(DT_NODELABEL(pup_uart)), .port = 0, .selected = -1},
	{.uart = DEVICE_DT_GET(DT_NODELABEL(pup_uart_b)), .port = 1, .selected = -1},
};
static K_MUTEX_DEFINE(peer_lock);

static uint8_t mode_count(const struct peer *peer)
{
	return peer->kind == FORCE ? 7 : peer->kind == TILT ? 1 : peer->kind == INFRARED ? 3 :
		peer->kind == COLOR ? ARRAY_SIZE(color_formats) :
		peer->kind == ULTRASONIC ? ARRAY_SIZE(ultrasonic_formats) :
		peer->kind == COLOR_DISTANCE ? ARRAY_SIZE(color_distance_formats) :
		peer->kind == MATRIX ? ARRAY_SIZE(matrix_formats) :
		peer->kind == ENCODED_MOTOR ? ARRAY_SIZE(motor_formats) : 4;
}

static const uint8_t *mode_format(const struct peer *peer, uint8_t mode)
{
	return peer->kind == FORCE ? force_formats[mode] :
		peer->kind == INFRARED ? infrared_formats[mode] :
		peer->kind == COLOR ? color_formats[mode] :
		peer->kind == ULTRASONIC ? ultrasonic_formats[mode] :
		peer->kind == COLOR_DISTANCE ? color_distance_formats[mode] :
		peer->kind == MATRIX ? matrix_formats[mode] :
		peer->kind == ENCODED_MOTOR ? motor_formats[mode] : NULL;
}

static int output_size(const struct peer *peer, uint8_t mode)
{
	return peer->kind == GENERIC && mode < 4 ? sample_lengths[mode] :
		peer->kind == COLOR && mode == 3 ? 3 :
		peer->kind == ULTRASONIC && mode == 5 ? 4 :
		peer->kind == COLOR_DISTANCE && mode == 5 ? 1 :
		peer->kind == COLOR_DISTANCE && mode == 7 ? 2 :
		peer->kind == MATRIX && mode == 2 ? 9 : 0;
}

static bool writable_mode(const struct peer *peer, uint8_t mode)
{
	return output_size(peer, mode) || peer->kind == MATRIX;
}

static void send_byte(struct peer *peer, uint8_t byte)
{
	uint32_t count = uart_emul_put_rx_data(peer->uart, &byte, 1);
	__ASSERT_NO_MSG(count == 1);
	ARG_UNUSED(count);
}

static void send_packet(struct peer *peer, uint8_t type, uint8_t mode, int info,
	const uint8_t *data, size_t length)
{
	uint8_t packet[35];
	size_t padded = 1, exponent = 0;
	while (padded < length) {
		padded <<= 1;
		exponent++;
	}
	__ASSERT_NO_MSG(padded <= 32);
	size_t count = 0;
	packet[count++] = type | (exponent << 3) | (mode & 7);
	if (info >= 0) packet[count++] = info | (mode >= 8 ? LUMP_INFO_MODE_PLUS_8 : 0);
	for (size_t i = 0; i < padded; i++) packet[count++] = i < length ? data[i] : 0;
	uint8_t checksum = 0xff;
	for (size_t i = 0; i < count; i++) checksum ^= packet[i];
	packet[count++] = checksum;
	uint32_t sent = uart_emul_put_rx_data(peer->uart, packet, count);
	__ASSERT_NO_MSG(sent == count);
	ARG_UNUSED(sent);
}

static void peer_work(struct k_work *item);

static void send_sample(struct peer *peer)
{
	static const uint8_t zero[32];
	static const uint8_t ultrasonic_presence[] = {1};
	static const uint8_t color_distance_proximity[] = {7};
	static const uint8_t color_distance_ambient[] = {25};
	uint8_t calib[4];
	const uint8_t *data = zero;
	size_t length = 1;
	if (peer->kind == GENERIC) {
		data = peer->has_written[peer->selected] ? peer->written[peer->selected] : samples[peer->selected];
		length = sample_lengths[peer->selected];
	} else if (peer->kind == FORCE) {
		if (peer->selected == 6) {
			data = force_calibration;
			length = sizeof(force_calibration);
		} else if (peer->selected == 4) {
			data = force_raw;
			length = sizeof(force_raw);
		}
	} else if (peer->kind == TILT) {
		data = tilt_raw;
		length = sizeof(tilt_raw);
	} else if (peer->kind == INFRARED && peer->selected == 1) {
		data = peer->infrared_count;
		length = sizeof(peer->infrared_count);
	} else if (peer->kind == INFRARED && peer->selected == 2) {
		data = infrared_cal;
		length = sizeof(infrared_cal);
	} else if (peer->kind == COLOR || peer->kind == ULTRASONIC ||
		   peer->kind == COLOR_DISTANCE || peer->kind == MATRIX ||
		   peer->kind == ENCODED_MOTOR) {
		const uint8_t *format = mode_format(peer, peer->selected);
		length = format[0] * (format[1] >= 2 ? 4 : 1U << format[1]);
		if (peer->kind == COLOR && peer->selected == 5) data = color_rgb;
		else if (peer->kind == COLOR && peer->selected == 7) data = color_shsv;
		else if (peer->kind == ULTRASONIC && peer->selected == 0) data = ultrasonic_distance;
		else if (peer->kind == ULTRASONIC && peer->selected == 3) data = ultrasonic_presence;
		else if (peer->kind == COLOR_DISTANCE && peer->selected == 1) data = color_distance_proximity;
		else if (peer->kind == COLOR_DISTANCE && peer->selected == 4) data = color_distance_ambient;
		else if (peer->kind == COLOR_DISTANCE && peer->selected == 6) data = color_distance_rgb;
		else if (peer->kind == ENCODED_MOTOR && peer->selected == 4) {
			peer->shaft_angle = (peer->shaft_angle + peer->shaft_step + 3600) % 3600;
			calib[0] = calib[1] = 0; /* First CALIB word is ignored by PBIO. */
			calib[2] = peer->shaft_angle;
			calib[3] = peer->shaft_angle >> 8;
			data = calib;
		}
	}
	send_packet(peer, LUMP_MSG_TYPE_DATA, peer->selected, -1, data, length);
}

static void peer_work(struct k_work *item)
{
	struct peer *peer = CONTAINER_OF(k_work_delayable_from_work(item), struct peer, work);
	k_mutex_lock(&peer_lock, K_FOREVER);
	if (!peer->connected || pup_test_circuits[peer->port].kind != UART_DEVICE) {
		k_mutex_unlock(&peer_lock);
		return;
	}
	if (peer->phase == SEND_METADATA) {
		if (peer->hold_reset) {
			/* Expose an incomplete handshake until the test releases metadata. */
			send_byte(peer, LUMP_SYS_SYNC);
			(void)k_work_reschedule(&peer->work, K_MSEC(50));
			k_mutex_unlock(&peer_lock);
			return;
		}
		const uint8_t id = peer->kind == FORCE ? 63 : peer->kind == TILT ? 34 :
			peer->kind == INFRARED ? 35 : peer->kind == COLOR ? 61 :
			peer->kind == ULTRASONIC ? 62 : peer->kind == COLOR_DISTANCE ? 37 :
			peer->kind == MATRIX ? 64 : peer->kind == ENCODED_MOTOR ? 46 : 201;
		uint8_t count = mode_count(peer);
		const uint8_t modes[] = {MIN(count - 1, 7), 0, count - 1, 0};
		const uint8_t speed[] = {0, 0xc2, 1, 0}; /* 115200 baud. */
		if (!peer->metadata_header_sent) {
			send_packet(peer, LUMP_MSG_TYPE_CMD, LUMP_CMD_TYPE, -1, &id, 1);
			send_packet(peer, LUMP_MSG_TYPE_CMD, LUMP_CMD_MODES, -1, modes, sizeof(modes));
			send_packet(peer, LUMP_MSG_TYPE_CMD, LUMP_CMD_SPEED, -1, speed, sizeof(speed));
			peer->metadata_header_sent = true;
		}
		for (uint8_t end = MIN(peer->metadata_mode + 2, count); peer->metadata_mode < end; peer->metadata_mode++) {
			uint8_t mode = peer->metadata_mode;
			const uint8_t name[] = {'R', 'A', 'W', mode < 10 ? (uint8_t)('0' + mode) : 'X'};
			const uint8_t calib_name[] = {'C', 'A', 'L', 'I', 'B', 0,
				LUMP_MODE_FLAGS0_MOTOR_ABS_POS | LUMP_MODE_FLAGS0_MOTOR, 0x40,
				0, 0, 5, 4};
			const uint8_t *known = mode_format(peer, mode);
			uint8_t values = peer->kind == GENERIC ? 2 : peer->kind == TILT ? 2 : known[0];
			uint8_t type = peer->kind == GENERIC ? mode : peer->kind == TILT ? 0 : known[1];
			const uint8_t format[] = {values, type, 6, 0};
			const uint8_t mapping[] = {writable_mode(peer, mode) ? 0 : 0x10,
				writable_mode(peer, mode) ? 1 : 0};
			send_packet(peer, LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_NAME,
				peer->kind == ENCODED_MOTOR && mode == 4 ? calib_name : name,
				peer->kind == ENCODED_MOTOR && mode == 4 ? sizeof(calib_name) : sizeof(name));
			if (peer->kind == GENERIC) {
				const uint8_t generic_mapping[] = {0x10, 0x10};
				send_packet(peer, LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_MAPPING,
					generic_mapping, sizeof(generic_mapping));
				if (mode == 3) {
					/* IEEE float32 -0.125 .. 123.5, independent wire fixture. */
					const uint8_t raw[] = {0, 0, 0, 0xbe, 0, 0, 0xf7, 0x42};
					send_packet(peer, LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_RAW, raw, sizeof(raw));
				}
			} else if (peer->kind == COLOR || peer->kind == ULTRASONIC ||
				   peer->kind == COLOR_DISTANCE || peer->kind == MATRIX) {
				send_packet(peer, LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_MAPPING, mapping, sizeof(mapping));
			}
			send_packet(peer, LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_FORMAT, format, sizeof(format));
		}
		if (peer->metadata_mode == count) {
			send_byte(peer, LUMP_SYS_ACK);
			peer->phase = WAIT_SPEED;
		} else {
			(void)k_work_reschedule(&peer->work, K_MSEC(2));
		}
	} else if (peer->phase == STREAM && peer->selected >= 0) {
		send_sample(peer);
		(void)k_work_reschedule(&peer->work, K_MSEC(20));
	}
	k_mutex_unlock(&peer_lock);
}

static void transmitted(const struct device *dev, size_t size, void *user_data)
{
	struct peer *peer = user_data;
	uint8_t bytes[16];
	k_mutex_lock(&peer_lock, K_FOREVER);
	while (size) {
		size_t count = uart_emul_get_tx_data(dev, bytes, MIN(size, sizeof(bytes)));
		if (!count) break;
		size -= count;
		if (peer->serial_mode) {
			/* Raw serial is a byte stream; never feed it to the LUMP parser. */
			uint32_t echoed = uart_emul_put_rx_data(dev, bytes, count);
			__ASSERT_NO_MSG(echoed == count);
			continue;
		}
		for (size_t i = 0; i < count && peer->connected; i++) {
			uint8_t byte = bytes[i];
			if (!peer->used) {
				peer->needed = byte == 0x52 ? 6 : byte == 0x43 || byte == 0x46 ? 3 :
					(byte & 0xc0) == 0xc0 ? (1 << ((byte >> 3) & 7)) + 2 : 0;
				if (peer->needed > sizeof(peer->command)) peer->needed = 0;
				if (!peer->needed) continue;
			}
			peer->command[peer->used++] = byte;
			if (peer->used != peer->needed) continue;
			peer->used = 0;
			uint8_t checksum = 0xff;
			for (size_t j = 0; j < peer->needed; j++) checksum ^= peer->command[j];
			if (checksum) continue;
			if (peer->needed == 6 && (peer->phase == WAIT_SPEED || peer->phase == STREAM) &&
			    !memcmp(peer->command, (const uint8_t[]){0x52, 0, 0xc2, 1, 0, 0x6e}, 6)) {
				/* The host offers fast baud for only 10 ms. The RX FIFO
				 * retains this ACK until OFFER_ACK handles it. */
				peer->selected = -1;
				peer->infrared_count[0] = 41;
				peer->extension_valid = false;
				peer->metadata_mode = 0;
				peer->metadata_header_sent = false;
				peer->has_output = false;
				memset(peer->has_written, 0, sizeof(peer->has_written));
				send_byte(peer, LUMP_SYS_ACK);
				peer->phase = SEND_METADATA;
				(void)k_work_reschedule(&peer->work, K_MSEC(2));
			} else if (peer->command[0] == 0x43 && peer->command[1] < mode_count(peer) &&
				   peer->command[2] == (uint8_t)(0xbc ^ peer->command[1])) {
				if (peer->hold_mode) continue;
				if (peer->kind == INFRARED && peer->selected == 1 && peer->command[1] == 2) {
					peer->infrared_count[0] = 46;
				}
				peer->selected = peer->command[1];
				peer->phase = STREAM;
				(void)k_work_reschedule(&peer->work, K_MSEC(2));
			} else if (peer->command[0] == 0x46 && (peer->command[1] == 0 || peer->command[1] == 8)) {
				peer->extension = peer->command[1];
				memcpy(peer->extension_packet, peer->command, sizeof(peer->extension_packet));
				peer->extension_valid = true;
			} else if ((peer->command[0] & 0xc0) == 0xc0 && peer->extension_valid) {
				uint8_t mode = peer->extension | (peer->command[0] & 7);
				peer->extension_valid = false;
				int size = output_size(peer, mode);
				if (!size || mode != peer->selected) continue;
				int padded = 1;
				while (padded < size) padded *= 2;
				if (peer->needed - 2 != padded) continue;
				peer->output_mode = mode;
				peer->output_length = sizeof(peer->extension_packet) + peer->needed;
				memcpy(peer->output_wire, peer->extension_packet, sizeof(peer->extension_packet));
				memcpy(peer->output_wire + sizeof(peer->extension_packet), peer->command, peer->needed);
				peer->has_output = true;
				if (peer->kind == COLOR_DISTANCE && mode == 7) {
					if (peer->pf_count < ARRAY_SIZE(peer->pf_history)) {
						peer->pf_history[peer->pf_count++] = (struct pf_record){
							.ms = k_uptime_get(),
							.message = peer->command[1] | (peer->command[2] << 8),
						};
					} else {
						peer->pf_dropped++;
					}
				}
				if (peer->kind == GENERIC) {
					memcpy(peer->written[mode], peer->command + 1, size);
					peer->has_written[mode] = true;
				}
				(void)k_work_reschedule(&peer->work, K_MSEC(2));
			}
		}
	}
	k_mutex_unlock(&peer_lock);
}

static int pup_peer(const struct shell *shell, size_t argc, char **argv)
{
	struct peer *peer = &peers[0];
	if (argc >= 3 && !strcmp(argv[1], "B")) {
		peer = &peers[1];
		argc--;
		argv++;
	}
	if (!device_is_ready(peer->uart)) return -ENODEV;
	if (!strcmp(argv[1], "serial_rx")) {
		uint8_t bytes[64];
		if (argc != 3) return -EINVAL;
		size_t length = strlen(argv[2]);
		if (!length || (length & 1) || length > sizeof(bytes) * 2 ||
		    hex2bin(argv[2], length, bytes, sizeof(bytes)) != length / 2) return -EINVAL;
		k_mutex_lock(&peer_lock, K_FOREVER);
		int err = peer->serial_mode ? 0 : -ENODEV;
		if (!err && uart_emul_put_rx_data(peer->uart, bytes, length / 2) != length / 2) err = -ENOSPC;
		k_mutex_unlock(&peer_lock);
		return err;
	}
	if (!strcmp(argv[1], "motor_angle") || !strcmp(argv[1], "motor_step")) {
		if (argc != 3) return -EINVAL;
		int err = 0;
		long value = shell_strtol(argv[2], 10, &err);
		bool angle = !strcmp(argv[1], "motor_angle");
		if (err || (angle ? value < 0 || value >= 3600 : value < -100 || value > 100)) return -EINVAL;
		k_mutex_lock(&peer_lock, K_FOREVER);
		err = peer->connected && peer->kind == ENCODED_MOTOR ? 0 : -ENODEV;
		if (!err) {
			if (angle) peer->shaft_angle = value;
			else peer->shaft_step = value;
		}
		k_mutex_unlock(&peer_lock);
		return err;
	}
	if (argc != 2) return -EINVAL;
	if (!strcmp(argv[1], "power")) {
		shell_print(shell, "supply=%u changes=%u", pup_test_sensor_supply(peer->port),
			pup_test_circuits[peer->port].supply_changes);
		return 0;
	}
	if (!strcmp(argv[1], "serial_power_error")) {
		pup_test_circuits[peer->port].supply_error = -EIO;
		return 0;
	}
	if (!strcmp(argv[1], "serial_close_error")) {
		k_mutex_lock(&peer_lock, K_FOREVER);
		pup_test_circuits[peer->port].coast_error = -EIO;
		k_mutex_unlock(&peer_lock);
		return 0;
	}
	if (!strcmp(argv[1], "output")) {
		static const char digits[] = "0123456789abcdef";
		char hex[sizeof(peer->output_wire) * 2 + 1];
		k_mutex_lock(&peer_lock, K_FOREVER);
		bool found = peer->has_output;
		uint8_t mode = peer->output_mode;
		for (uint8_t i = 0; found && i < peer->output_length; i++) {
			hex[2 * i] = digits[peer->output_wire[i] >> 4];
			hex[2 * i + 1] = digits[peer->output_wire[i] & 15];
		}
		if (found) hex[2 * peer->output_length] = 0;
		k_mutex_unlock(&peer_lock);
		if (found) shell_print(shell, "mode=%u bytes=%s", mode, hex);
		else shell_print(shell, "none");
		return 0;
	}
	if (!strcmp(argv[1], "pwm")) {
		shell_print(shell, "pwm channel=%u period=%u pulse=%u",
			fake_pwm_set_cycles_fake.arg1_val, fake_pwm_set_cycles_fake.arg2_val,
			fake_pwm_set_cycles_fake.arg3_val);
		return 0;
	}
	if (!strcmp(argv[1], "pf")) {
		struct pf_record records[PF_HISTORY_CAPACITY];
		k_mutex_lock(&peer_lock, K_FOREVER);
		uint8_t count = peer->pf_count;
		uint32_t dropped = peer->pf_dropped;
		memcpy(records, peer->pf_history, count * sizeof(records[0]));
		k_mutex_unlock(&peer_lock);
		shell_print(shell, "pf count=%u dropped=%u", count, dropped);
		for (uint8_t i = 0; i < count; i++) {
			shell_print(shell, "pf t=%lld value=%04x", (long long)records[i].ms,
				records[i].message);
		}
		return 0;
	}
	if (!strcmp(argv[1], "hold") || !strcmp(argv[1], "resume")) {
		k_mutex_lock(&peer_lock, K_FOREVER);
		peer->hold_mode = !strcmp(argv[1], "hold");
		k_mutex_unlock(&peer_lock);
		return 0;
	}
	if (!strcmp(argv[1], "hold_reset") || !strcmp(argv[1], "resume_reset")) {
		k_mutex_lock(&peer_lock, K_FOREVER);
		peer->hold_reset = !strcmp(argv[1], "hold_reset");
		if (!peer->hold_reset && peer->phase == SEND_METADATA) {
			(void)k_work_reschedule(&peer->work, K_NO_WAIT);
		}
		k_mutex_unlock(&peer_lock);
		return 0;
	}
	if (strcmp(argv[1], "connect") && strcmp(argv[1], "force") &&
	    strcmp(argv[1], "generic") && strcmp(argv[1], "serial") &&
	    strcmp(argv[1], "tilt") && strcmp(argv[1], "infrared") &&
	    strcmp(argv[1], "color") && strcmp(argv[1], "ultrasonic") &&
	    strcmp(argv[1], "color_distance") && strcmp(argv[1], "matrix") &&
	    strcmp(argv[1], "light") && strcmp(argv[1], "motor") &&
	    strcmp(argv[1], "encoded_motor") &&
	    strcmp(argv[1], "unplug") &&
	    strcmp(argv[1], "touch") && strcmp(argv[1], "press")) {
		shell_error(shell, "Expected a peer kind, encoded_motor, motor_angle, motor_step, serial, serial_rx, serial_close_error, unplug, touch, press, hold, resume, hold_reset, resume_reset, output, pwm or pf");
		return -EINVAL;
	}
	k_mutex_lock(&peer_lock, K_FOREVER);
	if (!peer->installed) {
		k_work_init_delayable(&peer->work, peer_work);
		uart_emul_callback_tx_data_ready_set(peer->uart, transmitted, peer);
		peer->installed = true;
	}
	peer->connected = false;
	peer->serial_mode = false;
	peer->hold_mode = false;
	peer->hold_reset = false;
	(void)k_work_cancel_delayable(&peer->work);
	(void)uart_emul_flush_rx_data(peer->uart);
	(void)uart_emul_flush_tx_data(peer->uart);
	peer->used = peer->needed = 0;
	peer->selected = -1;
	peer->kind = !strcmp(argv[1], "force") ? FORCE : !strcmp(argv[1], "tilt") ? TILT :
		!strcmp(argv[1], "infrared") ? INFRARED : !strcmp(argv[1], "color") ? COLOR :
		!strcmp(argv[1], "ultrasonic") ? ULTRASONIC :
		!strcmp(argv[1], "color_distance") ? COLOR_DISTANCE :
		!strcmp(argv[1], "matrix") ? MATRIX :
		!strcmp(argv[1], "encoded_motor") ? ENCODED_MOTOR : GENERIC;
	peer->shaft_angle = peer->shaft_step = 0;
	memset(peer->infrared_count, 0, sizeof(peer->infrared_count));
	peer->infrared_count[0] = 41;
	peer->phase = WAIT_SPEED;
	peer->metadata_mode = 0;
	peer->metadata_header_sent = false;
	memset(peer->has_written, 0, sizeof(peer->has_written));
	peer->extension_valid = false;
	peer->has_output = false;
	peer->pf_count = 0;
	peer->pf_dropped = 0;
	pup_test_circuits[peer->port].kind = !strcmp(argv[1], "light") ? LIGHT :
		!strcmp(argv[1], "motor") ? TRAIN_MOTOR :
		!strcmp(argv[1], "serial") ? UNPLUGGED :
		!strcmp(argv[1], "connect") || !strcmp(argv[1], "generic") ||
		peer->kind != GENERIC ? UART_DEVICE :
		!strcmp(argv[1], "unplug") ? UNPLUGGED : TOUCH_SWITCH;
	pup_test_circuits[peer->port].pressed = !strcmp(argv[1], "press");
	peer->connected = pup_test_circuits[peer->port].kind == UART_DEVICE;
	peer->serial_mode = !strcmp(argv[1], "serial");
	k_mutex_unlock(&peer_lock);
	return 0;
}
SHELL_CMD_ARG_REGISTER(pup_peer, NULL,
	"Synthetic port A (default) or B: [B] connect|generic|serial|serial_rx <hex>|serial_close_error|serial_power_error|power|force|tilt|infrared|color|ultrasonic|color_distance|matrix|light|motor|encoded_motor|motor_angle <0..3599>|motor_step <-100..100>|unplug|touch|press|hold|resume|hold_reset|resume_reset|output|pwm|pf.",
	pup_peer, 2, 2);
