// SPDX-License-Identifier: Apache-2.0
/* Synthetic LUMP peer for the protected Python port tests. Wire values are
 * chosen independently of the firmware's parser and conversion code. */
#include "../ports/wiring.h"

#include <errno.h>
#include <lego/lump.h>
#include <string.h>
#include <zephyr/drivers/pwm/pwm_fake.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(pup_uart));
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
static const uint8_t color_rgb[] = {0, 4, 0, 0, 0, 0, 0, 0}; /* 1024, 0, 0, 0. */
static const uint8_t color_shsv[] = {120, 0, 0xe8, 3, 0xe8, 3, 0, 0};
static const uint8_t ultrasonic_distance[] = {0x41, 1}; /* 321 mm. */
static const uint8_t color_distance_rgb[] = {0x90, 1, 0, 0, 0, 0}; /* 400, 0, 0. */
static enum { GENERIC, FORCE, TILT, INFRARED, COLOR, ULTRASONIC, COLOR_DISTANCE, MATRIX } peer_kind;
static uint8_t infrared_count[4];
static uint8_t written[4][8];
static bool has_written[4], extension_valid;
static uint8_t extension, extension_packet[3];
static uint8_t output_wire[37], output_mode, output_length;
static bool has_output;
static K_MUTEX_DEFINE(peer_lock);
static bool installed, connected, hold_mode, hold_reset;
static uint8_t command[34], used, needed;
static int selected = -1;
static uint8_t metadata_mode;
static bool metadata_header_sent;
static enum { WAIT_SPEED, SEND_METADATA, STREAM } phase;

static uint8_t mode_count(void)
{
	return peer_kind == FORCE ? 7 : peer_kind == TILT ? 1 : peer_kind == INFRARED ? 3 :
		peer_kind == COLOR ? ARRAY_SIZE(color_formats) :
		peer_kind == ULTRASONIC ? ARRAY_SIZE(ultrasonic_formats) :
		peer_kind == COLOR_DISTANCE ? ARRAY_SIZE(color_distance_formats) :
		peer_kind == MATRIX ? ARRAY_SIZE(matrix_formats) : 4;
}

static const uint8_t *mode_format(uint8_t mode)
{
	return peer_kind == FORCE ? force_formats[mode] :
		peer_kind == INFRARED ? infrared_formats[mode] :
		peer_kind == COLOR ? color_formats[mode] :
		peer_kind == ULTRASONIC ? ultrasonic_formats[mode] :
		peer_kind == COLOR_DISTANCE ? color_distance_formats[mode] :
		peer_kind == MATRIX ? matrix_formats[mode] : NULL;
}

static int output_size(uint8_t mode)
{
	return peer_kind == GENERIC && mode < 4 ? sample_lengths[mode] :
		peer_kind == COLOR && mode == 3 ? 3 :
		peer_kind == ULTRASONIC && mode == 5 ? 4 :
		peer_kind == COLOR_DISTANCE && mode == 5 ? 1 :
		peer_kind == MATRIX && mode == 2 ? 9 : 0;
}

static bool writable_mode(uint8_t mode)
{
	return output_size(mode) || peer_kind == MATRIX;
}

static void send_byte(uint8_t byte)
{
	uint32_t written = uart_emul_put_rx_data(uart, &byte, 1);
	__ASSERT_NO_MSG(written == 1);
	ARG_UNUSED(written);
}

static void send_packet(uint8_t type, uint8_t mode, int info, const uint8_t *data, size_t length)
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
	uint32_t written = uart_emul_put_rx_data(uart, packet, count);
	__ASSERT_NO_MSG(written == count);
	ARG_UNUSED(written);
}

static void peer_work(struct k_work *item);
static K_WORK_DELAYABLE_DEFINE(work, peer_work);

static void send_sample(void)
{
	static const uint8_t zero[32];
	static const uint8_t ultrasonic_presence[] = {1};
	static const uint8_t color_distance_proximity[] = {7};
	static const uint8_t color_distance_ambient[] = {25};
	const uint8_t *data = zero;
	size_t length = 1;
	if (peer_kind == GENERIC) {
		data = has_written[selected] ? written[selected] : samples[selected];
		length = sample_lengths[selected];
	} else if (peer_kind == FORCE) {
		if (selected == 6) {
			data = force_calibration;
			length = sizeof(force_calibration);
		} else if (selected == 4) {
			data = force_raw;
			length = sizeof(force_raw);
		}
	} else if (peer_kind == TILT) {
		data = tilt_raw;
		length = sizeof(tilt_raw);
	} else if (peer_kind == INFRARED && selected == 1) {
		data = infrared_count;
		length = sizeof(infrared_count);
	} else if (peer_kind == INFRARED && selected == 2) {
		data = infrared_cal;
		length = sizeof(infrared_cal);
	} else if (peer_kind == COLOR || peer_kind == ULTRASONIC ||
		   peer_kind == COLOR_DISTANCE || peer_kind == MATRIX) {
		const uint8_t *format = mode_format(selected);
		length = format[0] * (format[1] >= 2 ? 4 : 1U << format[1]);
		if (peer_kind == COLOR && selected == 5) data = color_rgb;
		else if (peer_kind == COLOR && selected == 7) data = color_shsv;
		else if (peer_kind == ULTRASONIC && selected == 0) data = ultrasonic_distance;
		else if (peer_kind == ULTRASONIC && selected == 3) data = ultrasonic_presence;
		else if (peer_kind == COLOR_DISTANCE && selected == 1) data = color_distance_proximity;
		else if (peer_kind == COLOR_DISTANCE && selected == 4) data = color_distance_ambient;
		else if (peer_kind == COLOR_DISTANCE && selected == 6) data = color_distance_rgb;
	}
	send_packet(LUMP_MSG_TYPE_DATA, selected, -1, data, length);
}

static void peer_work(struct k_work *item)
{
	ARG_UNUSED(item);
	k_mutex_lock(&peer_lock, K_FOREVER);
	if (!connected || pup_test_circuits[0].kind != UART_DEVICE) {
		k_mutex_unlock(&peer_lock);
		return;
	}
	if (phase == SEND_METADATA) {
		if (hold_reset) {
			/* Expose an incomplete handshake until the test releases metadata. */
			send_byte(LUMP_SYS_SYNC);
			(void)k_work_reschedule(&work, K_MSEC(50));
			k_mutex_unlock(&peer_lock);
			return;
		}
		const uint8_t id = peer_kind == FORCE ? 63 : peer_kind == TILT ? 34 :
			peer_kind == INFRARED ? 35 : peer_kind == COLOR ? 61 :
			peer_kind == ULTRASONIC ? 62 : peer_kind == COLOR_DISTANCE ? 37 :
			peer_kind == MATRIX ? 64 : 201;
		uint8_t count = mode_count();
		const uint8_t modes[] = {MIN(count - 1, 7), 0, count - 1, 0};
		const uint8_t speed[] = {0, 0xc2, 1, 0}; /* 115200 baud. */
		if (!metadata_header_sent) {
			send_packet(LUMP_MSG_TYPE_CMD, LUMP_CMD_TYPE, -1, &id, 1);
			send_packet(LUMP_MSG_TYPE_CMD, LUMP_CMD_MODES, -1, modes, sizeof(modes));
			send_packet(LUMP_MSG_TYPE_CMD, LUMP_CMD_SPEED, -1, speed, sizeof(speed));
			metadata_header_sent = true;
		}
		for (uint8_t end = MIN(metadata_mode + 2, count); metadata_mode < end; metadata_mode++) {
			uint8_t mode = metadata_mode;
			const uint8_t name[] = {'R', 'A', 'W', mode < 10 ? (uint8_t)('0' + mode) : 'X'};
			const uint8_t *known = mode_format(mode);
			uint8_t values = peer_kind == GENERIC ? 2 : peer_kind == TILT ? 2 : known[0];
			uint8_t type = peer_kind == GENERIC ? mode : peer_kind == TILT ? 0 : known[1];
			const uint8_t format[] = {values, type, 6, 0};
			const uint8_t mapping[] = {writable_mode(mode) ? 0 : 0x10,
				writable_mode(mode) ? 1 : 0};
			send_packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_NAME, name, sizeof(name));
			if (peer_kind == GENERIC) {
				const uint8_t generic_mapping[] = {0x10, 0x10};
				send_packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_MAPPING,
					generic_mapping, sizeof(generic_mapping));
			} else if (peer_kind == COLOR || peer_kind == ULTRASONIC ||
				   peer_kind == COLOR_DISTANCE || peer_kind == MATRIX) {
				send_packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_MAPPING, mapping, sizeof(mapping));
			}
			send_packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_FORMAT, format, sizeof(format));
		}
		if (metadata_mode == count) {
			send_byte(LUMP_SYS_ACK);
			phase = WAIT_SPEED;
		} else {
			(void)k_work_reschedule(&work, K_MSEC(2));
		}
	} else if (phase == STREAM && selected >= 0) {
		send_sample();
		(void)k_work_reschedule(&work, K_MSEC(20));
	}
	k_mutex_unlock(&peer_lock);
}

static void transmitted(const struct device *dev, size_t size, void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t bytes[16];
	k_mutex_lock(&peer_lock, K_FOREVER);
	while (size) {
		size_t count = uart_emul_get_tx_data(dev, bytes, MIN(size, sizeof(bytes)));
		if (!count) break;
		size -= count;
		for (size_t i = 0; i < count && connected; i++) {
			uint8_t byte = bytes[i];
			if (!used) {
				needed = byte == 0x52 ? 6 : byte == 0x43 || byte == 0x46 ? 3 :
					(byte & 0xc0) == 0xc0 ? (1 << ((byte >> 3) & 7)) + 2 : 0;
				if (needed > sizeof(command)) needed = 0;
				if (!needed) continue;
			}
			command[used++] = byte;
			if (used != needed) continue;
			used = 0;
			uint8_t checksum = 0xff;
			for (size_t j = 0; j < needed; j++) checksum ^= command[j];
			if (checksum) continue;
			if (needed == 6 && (phase == WAIT_SPEED || phase == STREAM) &&
			    !memcmp(command, (const uint8_t[]){0x52, 0, 0xc2, 1, 0, 0x6e}, 6)) {
				/* The host offers fast baud for only 10 ms. The RX FIFO
				 * retains this ACK until OFFER_ACK handles it. */
				selected = -1;
				infrared_count[0] = 41;
				extension_valid = false;
				metadata_mode = 0;
				metadata_header_sent = false;
				has_output = false;
				memset(has_written, 0, sizeof(has_written));
				send_byte(LUMP_SYS_ACK);
				phase = SEND_METADATA;
				(void)k_work_reschedule(&work, K_MSEC(2));
			} else if (command[0] == 0x43 && command[1] < mode_count() &&
				   command[2] == (uint8_t)(0xbc ^ command[1])) {
				if (hold_mode) continue;
				if (peer_kind == INFRARED && selected == 1 && command[1] == 2) {
					infrared_count[0] = 46;
				}
				selected = command[1];
				phase = STREAM;
				(void)k_work_reschedule(&work, K_MSEC(2));
			} else if (command[0] == 0x46 && (command[1] == 0 || command[1] == 8)) {
				extension = command[1];
				memcpy(extension_packet, command, sizeof(extension_packet));
				extension_valid = true;
			} else if ((command[0] & 0xc0) == 0xc0 && extension_valid) {
				uint8_t mode = extension | (command[0] & 7);
				extension_valid = false;
				int size = output_size(mode);
				if (!size || mode != selected) continue;
				int padded = 1;
				while (padded < size) padded *= 2;
				if (needed - 2 != padded) continue;
				output_mode = mode;
				output_length = sizeof(extension_packet) + needed;
				memcpy(output_wire, extension_packet, sizeof(extension_packet));
				memcpy(output_wire + sizeof(extension_packet), command, needed);
				has_output = true;
				if (peer_kind == GENERIC) {
					memcpy(written[mode], command + 1, size);
					has_written[mode] = true;
				}
				(void)k_work_reschedule(&work, K_MSEC(2));
			}
		}
	}
	k_mutex_unlock(&peer_lock);
}

static int pup_peer(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (!device_is_ready(uart)) return -ENODEV;
	if (!strcmp(argv[1], "output")) {
		static const char digits[] = "0123456789abcdef";
		char hex[sizeof(output_wire) * 2 + 1];
		k_mutex_lock(&peer_lock, K_FOREVER);
		bool found = has_output;
		uint8_t mode = output_mode;
		for (uint8_t i = 0; found && i < output_length; i++) {
			hex[2 * i] = digits[output_wire[i] >> 4];
			hex[2 * i + 1] = digits[output_wire[i] & 15];
		}
		if (found) hex[2 * output_length] = 0;
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
	if (!strcmp(argv[1], "hold") || !strcmp(argv[1], "resume")) {
		k_mutex_lock(&peer_lock, K_FOREVER);
		hold_mode = !strcmp(argv[1], "hold");
		k_mutex_unlock(&peer_lock);
		return 0;
	}
	if (!strcmp(argv[1], "hold_reset") || !strcmp(argv[1], "resume_reset")) {
		k_mutex_lock(&peer_lock, K_FOREVER);
		hold_reset = !strcmp(argv[1], "hold_reset");
		if (!hold_reset && phase == SEND_METADATA) {
			(void)k_work_reschedule(&work, K_NO_WAIT);
		}
		k_mutex_unlock(&peer_lock);
		return 0;
	}
	if (strcmp(argv[1], "connect") && strcmp(argv[1], "force") &&
	    strcmp(argv[1], "tilt") && strcmp(argv[1], "infrared") &&
	    strcmp(argv[1], "color") && strcmp(argv[1], "ultrasonic") &&
	    strcmp(argv[1], "color_distance") && strcmp(argv[1], "matrix") &&
	    strcmp(argv[1], "light") &&
	    strcmp(argv[1], "unplug") &&
	    strcmp(argv[1], "touch") && strcmp(argv[1], "press")) {
		shell_error(shell, "Expected a peer kind, unplug, touch, press, hold, resume, hold_reset, resume_reset, output or pwm");
		return -EINVAL;
	}
	k_mutex_lock(&peer_lock, K_FOREVER);
	if (!installed) {
		uart_emul_callback_tx_data_ready_set(uart, transmitted, NULL);
		installed = true;
	}
	connected = false;
	hold_mode = false;
	hold_reset = false;
	(void)k_work_cancel_delayable(&work);
	(void)uart_emul_flush_rx_data(uart);
	(void)uart_emul_flush_tx_data(uart);
	used = needed = 0;
	selected = -1;
	peer_kind = !strcmp(argv[1], "force") ? FORCE : !strcmp(argv[1], "tilt") ? TILT :
		!strcmp(argv[1], "infrared") ? INFRARED : !strcmp(argv[1], "color") ? COLOR :
		!strcmp(argv[1], "ultrasonic") ? ULTRASONIC :
		!strcmp(argv[1], "color_distance") ? COLOR_DISTANCE :
		!strcmp(argv[1], "matrix") ? MATRIX : GENERIC;
	memset(infrared_count, 0, sizeof(infrared_count));
	infrared_count[0] = 41;
	phase = WAIT_SPEED;
	metadata_mode = 0;
	metadata_header_sent = false;
	memset(has_written, 0, sizeof(has_written));
	extension_valid = false;
	has_output = false;
	pup_test_circuits[0].kind = !strcmp(argv[1], "light") ? LIGHT :
		!strcmp(argv[1], "connect") || peer_kind != GENERIC ? UART_DEVICE :
		!strcmp(argv[1], "unplug") ? UNPLUGGED : TOUCH_SWITCH;
	pup_test_circuits[0].pressed = !strcmp(argv[1], "press");
	connected = pup_test_circuits[0].kind == UART_DEVICE;
	k_mutex_unlock(&peer_lock);
	return 0;
}
SHELL_CMD_ARG_REGISTER(pup_peer, NULL,
	"Synthetic port A: connect|force|tilt|infrared|color|ultrasonic|color_distance|matrix|light|unplug|touch|press|hold|resume|hold_reset|resume_reset|output|pwm.",
	pup_peer, 2, 0);
