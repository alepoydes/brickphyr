// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors
#pragma once
#include "lump.h"
#include <brickphyr_sensor.h>

/* Private state, exclusively owned by the existing port worker. */
struct port_sensor {
	struct bp_sensor_sample value;
	int64_t first_ms, deadline;
	int16_t offset, released, end;
	int32_t previous_angle;
	uint8_t mode;
	bool pending, awaiting, calibrated, angle_initialized;
};
void port_sensor_reset(struct port_sensor *s, const struct lump *l);
int port_sensor_request(struct port_sensor *s, const struct lump *l, uint32_t measurement);
void port_sensor_sent(struct port_sensor *s, int64_t now);
void port_sensor_receive(struct port_sensor *s, const struct lump *l, int64_t now);
