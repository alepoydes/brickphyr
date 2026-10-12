// SPDX-License-Identifier: Apache-2.0
/* Synthetic LUMP peer for the protected Python port tests. Wire values are
 * chosen independently of the firmware's parser and conversion code. */
#include "../ports/wiring.h"

#include <errno.h>
#include <lego/lump.h>
#include <string.h>
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
static uint8_t written[4][8];
static bool has_written[4], extension_valid;
static uint8_t extension;
static K_MUTEX_DEFINE(peer_lock);
static bool installed, connected, hold_mode;
static uint8_t command[34], used, needed;
static int selected = -1;
static enum { WAIT_SPEED, SEND_METADATA, STREAM } phase;

static void send_byte(uint8_t byte)
{
	uint32_t written = uart_emul_put_rx_data(uart, &byte, 1);
	__ASSERT_NO_MSG(written == 1);
	ARG_UNUSED(written);
}

static void send_packet(uint8_t type, uint8_t mode, int info, const uint8_t *data, size_t length)
{
	uint8_t packet[12];
	size_t padded = 1, exponent = 0;
	while (padded < length) {
		padded <<= 1;
		exponent++;
	}
	__ASSERT_NO_MSG(padded <= 8);
	size_t count = 0;
	packet[count++] = type | (exponent << 3) | mode;
	if (info >= 0) packet[count++] = info;
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

static void peer_work(struct k_work *item)
{
	ARG_UNUSED(item);
	k_mutex_lock(&peer_lock, K_FOREVER);
	if (!connected || pup_test_circuits[0].kind != UART_DEVICE) {
		k_mutex_unlock(&peer_lock);
		return;
	}
	if (phase == SEND_METADATA) {
		const uint8_t id = 201;
		const uint8_t modes[] = {3, 0, 3, 0};
		const uint8_t speed[] = {0, 0xc2, 1, 0}; /* 115200 baud. */
		send_packet(LUMP_MSG_TYPE_CMD, LUMP_CMD_TYPE, -1, &id, 1);
		send_packet(LUMP_MSG_TYPE_CMD, LUMP_CMD_MODES, -1, modes, sizeof(modes));
		send_packet(LUMP_MSG_TYPE_CMD, LUMP_CMD_SPEED, -1, speed, sizeof(speed));
		for (uint8_t mode = 0; mode < 4; mode++) {
			const uint8_t name[] = {'R', 'A', 'W', (uint8_t)('0' + mode)};
			const uint8_t format[] = {2, mode, 6, 0};
			const uint8_t mapping[] = {0x10, 0x10};
			send_packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_NAME, name, sizeof(name));
			send_packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_MAPPING, mapping, sizeof(mapping));
			send_packet(LUMP_MSG_TYPE_INFO, mode, LUMP_INFO_FORMAT, format, sizeof(format));
		}
		send_byte(LUMP_SYS_ACK);
		phase = WAIT_SPEED;
	} else if (phase == STREAM && selected >= 0) {
		send_packet(LUMP_MSG_TYPE_DATA, selected, -1,
			has_written[selected] ? written[selected] : samples[selected], sample_lengths[selected]);
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
			if (needed == 6 && phase == WAIT_SPEED &&
			    !memcmp(command, (const uint8_t[]){0x52, 0, 0xc2, 1, 0, 0x6e}, 6)) {
				/* The host offers fast baud for only 10 ms. The RX FIFO
				 * retains this ACK until OFFER_ACK handles it. */
				send_byte(LUMP_SYS_ACK);
				phase = SEND_METADATA;
				(void)k_work_reschedule(&work, K_MSEC(2));
			} else if (command[0] == 0x43 && command[1] < 4 &&
				   command[2] == (uint8_t)(0xbc ^ command[1])) {
				if (hold_mode) continue;
				selected = command[1];
				phase = STREAM;
				(void)k_work_reschedule(&work, K_MSEC(2));
			} else if (command[0] == 0x46 && (command[1] == 0 || command[1] == 8)) {
				extension = command[1];
				extension_valid = true;
			} else if ((command[0] & 0xc0) == 0xc0 && extension_valid) {
				uint8_t mode = extension | (command[0] & 7);
				extension_valid = false;
				if (mode >= 4 || mode != selected || needed - 2 != sample_lengths[mode]) continue;
				memcpy(written[mode], command + 1, sample_lengths[mode]);
				has_written[mode] = true;
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
	if (!strcmp(argv[1], "hold") || !strcmp(argv[1], "resume")) {
		k_mutex_lock(&peer_lock, K_FOREVER);
		hold_mode = !strcmp(argv[1], "hold");
		k_mutex_unlock(&peer_lock);
		return 0;
	}
	if (strcmp(argv[1], "connect") && strcmp(argv[1], "unplug") &&
	    strcmp(argv[1], "touch") && strcmp(argv[1], "press")) {
		shell_error(shell, "Expected connect, unplug, touch, press, hold or resume");
		return -EINVAL;
	}
	k_mutex_lock(&peer_lock, K_FOREVER);
	if (!installed) {
		uart_emul_callback_tx_data_ready_set(uart, transmitted, NULL);
		installed = true;
	}
	connected = false;
	hold_mode = false;
	(void)k_work_cancel_delayable(&work);
	(void)uart_emul_flush_rx_data(uart);
	(void)uart_emul_flush_tx_data(uart);
	used = needed = 0;
	selected = -1;
	phase = WAIT_SPEED;
	memset(has_written, 0, sizeof(has_written));
	extension_valid = false;
	pup_test_circuits[0].kind = !strcmp(argv[1], "connect") ? UART_DEVICE :
		!strcmp(argv[1], "unplug") ? UNPLUGGED : TOUCH_SWITCH;
	pup_test_circuits[0].pressed = !strcmp(argv[1], "press");
	connected = pup_test_circuits[0].kind == UART_DEVICE;
	k_mutex_unlock(&peer_lock);
	return 0;
}
SHELL_CMD_ARG_REGISTER(pup_peer, NULL, "Synthetic port A: connect|unplug|touch|press|hold|resume.",
	pup_peer, 2, 0);
