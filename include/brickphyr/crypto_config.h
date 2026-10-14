// SPDX-License-Identifier: Apache-2.0
#ifndef BRICKPHYR_CRYPTO_CONFIG_H
#define BRICKPHYR_CRYPTO_CONFIG_H

#include <zephyr/kernel.h>

/* Use TF-PSA's public allocator configuration for all temporary crypto data.
 * No runtime allocator replacement or separate libc/crypto arena is needed. */
#undef MBEDTLS_MEMORY_BUFFER_ALLOC_C
#define MBEDTLS_PLATFORM_CALLOC_MACRO k_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO k_free

#endif
