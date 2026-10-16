// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <brickphyr_control.h>

/* Shared Shell grammar: duration_ms followed by A=duty|brake|coast members.
 * Pure parsing: no clock, generation lookup, claims or device writes. */
int motor_command_parse(size_t argc, char **argv,
	struct bp_motor_group_command *command, uint32_t *duration_ms);
