# SPDX-License-Identifier: Apache-2.0
include_guard(GLOBAL)

if(CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE STREQUAL "brickphyr/crypto_config.h")
  zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/../include)
  # TF-PSA needs Annex K types before any libc header, including Zephyr callers.
  zephyr_compile_definitions(__STDC_WANT_LIB_EXT1__=1)
endif()

if(CONFIG_FILE_SYSTEM_LITTLEFS)
  # All mounts supply buffers; Zephyr supplies each opened file's cache.
  # Remove LittleFS's unused libc allocator and direct file-open alternative.
  zephyr_compile_definitions(LFS_NO_MALLOC)
endif()
