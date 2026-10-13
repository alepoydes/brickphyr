// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#define BP_ABI_MAGIC 0x31415042U /* BPA1 */
#define BP_ABI_MAJOR 1
#define BP_ABI_MINOR 0

/* ELF EABI5, little endian, Thumb, 32-bit pointers and AAPCS. */
#if defined(__ARM_ARCH_7EM__) && defined(__ARM_PCS_VFP) && (__ARM_FP == 4)
#define BP_ABI_ARCH 1U /* ARMv7E-M, FPv4-SP-D16, hard float */
#define BRICKPHYR_ABI "bp1.0-armv7em-hf"
#elif defined(__ARM_ARCH_7M__) && !defined(__ARM_PCS_VFP)
#define BP_ABI_ARCH 2U /* ARMv7-M, software float; simulator fixture */
#define BRICKPHYR_ABI "bp1.0-armv7m-sf"
#else
#error Unsupported Brickphyr native architecture
#endif

/* Available services, not a claim that an attached device is ready. */
enum bp_capability {
	BP_CAP_LIGHT = 1U << 0,
	BP_CAP_MATRIX = 1U << 1,
	BP_CAP_IMU = 1U << 2,
	BP_CAP_SOUND = 1U << 3,
	BP_CAP_BATTERY = 1U << 4,
	BP_CAP_CHARGER = 1U << 5,
	BP_CAP_PORTS = 1U << 6,
	BP_CAP_PORT_UART = 1U << 7,
	BP_CAP_MOTORS = 1U << 8,
	BP_CAP_MOTOR_CONTROL = 1U << 9,
	BP_CAP_RADIO = 1U << 10,
	BP_CAP_BLE_PEER = 1U << 11,
	BP_CAP_HUB_NETWORK = 1U << 12,
	BP_CAP_APP_STORAGE = 1U << 13,
};

struct bp_application_abi {
	uint32_t magic;
	uint16_t major;
	uint16_t minor;
	uint32_t architecture;
	uint32_t capabilities;
	uint32_t stack_size;
};

/* Admission precedes replacement. Declare indispensable services and the
 * application's stack requirement; optional services can be queried at runtime.
 * These declarations do not grant privileges or reserve peripheral ownership. */
#define BRICKPHYR_APPLICATION_REQUIRES(features, stack_bytes) \
	static const struct bp_application_abi \
	__attribute__((section(".brickphyr.abi"), used)) brickphyr_application_abi = { \
		BP_ABI_MAGIC, BP_ABI_MAJOR, BP_ABI_MINOR, BP_ABI_ARCH, features, stack_bytes \
	}
#define BRICKPHYR_APPLICATION() BRICKPHYR_APPLICATION_REQUIRES(0, 2048)
