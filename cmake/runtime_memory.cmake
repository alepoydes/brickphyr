# SPDX-License-Identifier: Apache-2.0
include_guard(GLOBAL)

if(CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE STREQUAL "brickphyr/crypto_config.h")
  zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../include)
endif()

if(CONFIG_FILE_SYSTEM_LITTLEFS)
  # All mounts supply buffers; Zephyr supplies each opened file's cache.
  # Remove LittleFS's unused libc allocator and direct file-open alternative.
  zephyr_compile_definitions(LFS_NO_MALLOC)
endif()
