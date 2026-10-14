// SPDX-License-Identifier: Apache-2.0
#include "application.h"
#include "native_image.h"

#include <errno.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <brickphyr_abi.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/llext/elf.h>
#include <zephyr/llext/buf_loader.h>

#ifdef CONFIG_FPU
static const unsigned char fpu[] = {
#include <native_fpu.inc>
};
#endif
#ifdef CONFIG_CMSIS_DSP_FILTERING
static const unsigned char dsp[] = {
#include <native_dsp.inc>
};
#endif
#ifdef NATIVE_CAPACITY_TEST
/* Legal trailing padding exercises a maximum-size upload independently of
 * code density; the actual application has a 64 KiB BSS. */
static const unsigned char capacity[APPLICATION_IMAGE_LIMIT] = {
#include <native_capacity.inc>
};
const void *native_fixture_capacity(size_t *length)
{
	*length = sizeof(capacity);
	return capacity;
}
#endif
static const unsigned char hello[] = {
#include <native_hello.inc>
};
static const unsigned char counter[] = {
#include <native_counter.inc>
};
static const unsigned char spin[] = {
#include <native_spin.inc>
};
static const unsigned char fault[] = {
#include <native_fault.inc>
};
static const unsigned char matrix[] = {
#include <native_matrix.inc>
};
static const unsigned char matrix_fault[] = {
#include <native_matrix_fault.inc>
};
static const unsigned char light_fault[] = {
#include <native_light_fault.inc>
};
static const unsigned char imu[] = {
#include <native_imu.inc>
};
static const unsigned char sound[] = {
#include <native_sound.inc>
};
static const unsigned char sound_fault[] = {
#include <native_sound_fault.inc>
};
static const unsigned char battery_fault[] = {
#include <native_battery_fault.inc>
};
static const unsigned char system_fault[] = {
#include <native_system_fault.inc>
};
static const unsigned char unsupported[] = {
#include <native_unsupported.inc>
};
static const unsigned char motor[] = {
#include <native_motor.inc>
};

const void *native_fixture_hello(size_t *length)
{
	*length = sizeof(hello);
	return hello;
}

int native_fixture_run(const char *name)
{
	if (!strcmp(name, "metadata") || !strcmp(name, "sparse")) {
		bool sparse = !strcmp(name, "sparse");
		const unsigned char *original = sparse ? spin : hello;
		size_t original_size = sparse ? sizeof(spin) : sizeof(hello);
		size_t length = sparse ? APPLICATION_IMAGE_LIMIT : original_size + 1024;
		unsigned char *changed = k_malloc(length);
		if (!changed) {
			return -ENOBUFS;
		}
		memcpy(changed, original, original_size);
		memset(changed + original_size, 0, length - original_size);
		elf_ehdr_t *header = (void *)changed;
		elf_shdr_t *sections = (void *)(changed + header->e_shoff);
		const char *names = (void *)(changed + sections[header->e_shstrndx].sh_offset);
		int err = -ENOENT;
		for (size_t i = 0; i < header->e_shnum; i++) {
			if (!strcmp(names + sections[i].sh_name, ".comment")) {
				/* A larger comment is not resident. A second code section
				 * at the same file boundary makes the whole gap resident. */
				sections[i].sh_offset = sparse ? length - 4 : original_size;
				sections[i].sh_size = sparse ? 4 : 1024;
				if (sparse) {
					sections[i].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
					sections[i].sh_addralign = 4;
					sections[i].sh_entsize = 0;
				}
				if (sparse) {
					/* Isolate admission: a later backing-allocation error
					 * must not pass for rejecting the file's memory cost. */
					struct llext_buf_loader loader = LLEXT_TEMPORARY_BUF_LOADER(changed, length);
					size_t heap;
					err = native_image_check(&loader.loader, length, &heap);
				} else {
					err = application_run_native(changed, length);
				}
				break;
			}
		}
		k_free(changed);
		return err;
	}
	if (!strcmp(name, "badimport") || !strcmp(name, "badreloc")) {
		unsigned char *changed = k_malloc(sizeof(hello));
		if (!changed) return -ENOMEM;
		memcpy(changed, hello, sizeof(hello));
		elf_ehdr_t *header = (void *)changed;
		elf_shdr_t *sections = (void *)(changed + header->e_shoff);
		bool found = false;
		for (size_t i = 0; i < header->e_shnum && !found; i++) {
			if (sections[i].sh_type != SHT_REL) continue;
			elf_shdr_t *symtab = &sections[sections[i].sh_link];
			elf_sym_t *symbols = (void *)(changed + symtab->sh_offset);
			char *names = (void *)(changed + sections[symtab->sh_link].sh_offset);
			elf_rel_t *relocations = (void *)(changed + sections[i].sh_offset);
			for (size_t j = 0; j < sections[i].sh_size / sizeof(*relocations); j++) {
				elf_rel_t *rel = &relocations[j];
				elf_sym_t *symbol = &symbols[ELF_R_SYM(rel->r_info)];
				if (symbol->st_shndx != SHN_UNDEF) continue;
				if (!strcmp(name, "badimport")) {
					memcpy(names + symbol->st_name, "k_sleep", sizeof("k_sleep"));
				} else {
					rel->r_info = (rel->r_info & ~0xffU) | 10; /* R_ARM_THM_CALL */
				}
				found = true;
				break;
			}
		}
		int err = found ? application_run_native(changed, sizeof(hello)) : -ENOENT;
		k_free(changed);
		return err;
	}
	if (!strcmp(name, "badfloat") || !strcmp(name, "unwind")) {
		unsigned char *changed = k_malloc(sizeof(hello));
		if (!changed) {
			return -ENOMEM;
		}
		memcpy(changed, hello, sizeof(hello));
		if (!strcmp(name, "badfloat")) {
			/* ELF32 e_flags: keep EABI version, invert the explicit float ABI. */
			uint32_t flags = sys_get_le32(changed + 36) & ~0x600U;
			flags |= IS_ENABLED(CONFIG_FP_HARDABI) ? 0x200U : 0x400U;
			sys_put_le32(flags, changed + 36);
		} else {
			elf_ehdr_t *header = (void *)changed;
			elf_shdr_t *sections = (void *)(changed + header->e_shoff);
			for (size_t i = 0; i < header->e_shnum; i++) {
				if (sections[i].sh_flags & SHF_ALLOC) {
					sections[i].sh_type = 0x70000001; /* SHT_ARM_EXIDX */
					break;
				}
			}
		}
		int err = application_run_native(changed, sizeof(hello));
		k_free(changed);
		return err;
	}
	if (!strcmp(name, "badabi") || !strcmp(name, "missingcap") || !strcmp(name, "bigstack")) {
		unsigned char *changed = k_malloc(sizeof(hello));
		if (!changed) {
			return -ENOMEM;
		}
		memcpy(changed, hello, sizeof(hello));
		int err = -ENOENT;
		elf_ehdr_t *header = (void *)changed;
		elf_shdr_t *sections = (void *)(changed + header->e_shoff);
		const char *names = (void *)(changed + sections[header->e_shstrndx].sh_offset);
		for (size_t i = 0; i < header->e_shnum; i++) {
			if (!strcmp(names + sections[i].sh_name, ".brickphyr.abi")) {
				struct bp_application_abi *abi = (void *)(changed + sections[i].sh_offset);
				if (!strcmp(name, "badabi")) {
					abi->major++;
				} else if (!strcmp(name, "missingcap")) {
					abi->capabilities = 1U << 31;
				} else {
					abi->stack_size = UINT32_MAX;
				}
				err = application_run_native(changed, sizeof(hello));
				break;
			}
		}
		k_free(changed);
		return err;
	}
	static const struct {
		const char *name;
		const void *data;
		size_t length;
	} images[] = {
#ifdef CONFIG_FPU
		{"fpu", fpu, sizeof(fpu)},
#endif
#ifdef CONFIG_CMSIS_DSP_FILTERING
		{"dsp", dsp, sizeof(dsp)},
#endif
		{"hello", hello, sizeof(hello)}, {"counter", counter, sizeof(counter)},
		{"spin", spin, sizeof(spin)}, {"fault", fault, sizeof(fault)},
		{"matrix", matrix, sizeof(matrix)}, {"matrix_fault", matrix_fault, sizeof(matrix_fault)},
		{"light_fault", light_fault, sizeof(light_fault)},
		{"imu", imu, sizeof(imu)},
		{"sound", sound, sizeof(sound)}, {"sound_fault", sound_fault, sizeof(sound_fault)},
		{"battery_fault", battery_fault, sizeof(battery_fault)},
		{"system_fault", system_fault, sizeof(system_fault)},
		{"unsupported", unsupported, sizeof(unsupported)},
		{"motor", motor, sizeof(motor)},
		{"truncated", hello, sizeof(hello) - 1},
	};
	for (size_t i = 0; i < ARRAY_SIZE(images); i++) {
		if (!strcmp(name, images[i].name)) {
			return application_run_native(images[i].data, images[i].length);
		}
	}
	return -EINVAL;
}

static int cmd_fixture(const struct shell *shell, size_t argc, char **argv)
{
	int err = native_fixture_run(argv[1]);
	shell_print(shell, "Load %s: %d", argv[1], err);
	return err;
}

SHELL_CMD_ARG_REGISTER(native_fixture, NULL, "Board test: hello, counter, spin, fault, matrix, matrix_fault, light_fault, imu, sound, sound_fault, battery_fault, motor, truncated, badabi.",
		      cmd_fixture, 2, 0);
