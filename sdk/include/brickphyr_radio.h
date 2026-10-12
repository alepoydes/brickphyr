// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

/* Legacy Pybricks manufacturer payload, excluding company/channel headers. */
#define BP_RADIO_DATA_MAX 26
#define BP_RADIO_CHANNELS_MAX 255
#define BP_RADIO_VERSION_MAX 32
struct bp_radio_sample {
	uint32_t timestamp_ms;
	int8_t rssi;
	uint8_t size;
	uint8_t valid;
	uint8_t data[BP_RADIO_DATA_MAX];
};
