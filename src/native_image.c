// SPDX-License-Identifier: Apache-2.0
#include "native_image.h"

#include <brickphyr_abi.h>
#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>

/* The initial SDK emits ELF32 ARM relocatable objects. Bound parser work and
 * memory before giving an untrusted image to the privileged linker. */
#define HEAP_LIMIT (64 * 1024)
#define SECTION_LIMIT 64

struct image {
	struct llext_loader *loader;
	size_t length;
	elf_ehdr_t header;
};

static bool range(size_t offset, size_t count, size_t length)
{
	return offset <= length && count <= length - offset;
}

static int read_at(struct image *image, size_t offset, void *data, size_t length)
{
	if (!range(offset, length, image->length)) {
		return -ENOEXEC;
	}
	int err = image->loader->seek(image->loader, offset);
	return err ? err : image->loader->read(image->loader, data, length);
}

static int section(struct image *image, unsigned int index, elf_shdr_t *header)
{
	if (index >= image->header.e_shnum) {
		return -ENOEXEC;
	}
	return read_at(image, image->header.e_shoff + index * sizeof(*header),
		       header, sizeof(*header));
}

static int name_at(struct image *image, const elf_shdr_t *strings, size_t offset,
		   char *name, size_t capacity)
{
	if (offset >= strings->sh_size) {
		return -ENOEXEC;
	}
	size_t length = MIN(capacity, strings->sh_size - offset);
	int err = read_at(image, strings->sh_offset + offset, name, length);
	return err ? err : memchr(name, 0, length) ? 0 : -ENOEXEC;
}

int native_image_check(struct llext_loader *loader, size_t length, size_t *heap_size)
{
	if (length > NATIVE_IMAGE_LIMIT) {
		return -EFBIG;
	}
	struct image image = {.loader = loader, .length = length};
	elf_ehdr_t *header = &image.header;
	int err = read_at(&image, 0, header, sizeof(*header));
	if (err) {
		return err;
	}
	if (memcmp(header->e_ident, "\177ELF\1\1\1", 7) || header->e_type != ET_REL ||
	    header->e_machine != 40 /* EM_ARM */ || header->e_version != 1 /* EV_CURRENT */ ||
	    header->e_ehsize != sizeof(*header) || header->e_phnum ||
	    header->e_shentsize != sizeof(elf_shdr_t) || !header->e_shnum ||
	    header->e_shnum > SECTION_LIMIT ||
	    !range(header->e_shoff, header->e_shnum * sizeof(elf_shdr_t), length)) {
		return -ENOEXEC;
	}
	/* EABI5, soft-float call convention, as emitted by the matching F4 SDK. */
	if ((header->e_flags & 0xff000400U) != 0x05000000U) {
		return -ENOTSUP;
	}
	elf_shdr_t names;
	err = section(&image, header->e_shstrndx, &names);
	if (err || names.sh_type != SHT_STRTAB ||
	    !range(names.sh_offset, names.sh_size, length)) {
		return err ? err : -ENOEXEC;
	}
	bool abi = false;
	bool entry = false;
	unsigned int symtabs = 0;
	/* Header/map/symbol bookkeeping plus allocator metadata. Section alignment
	 * can consume almost another whole block. This is a bounded allocation
	 * budget, not a claim about actual occupied bytes in LLEXT's heap. */
	size_t budget = 4096 + header->e_shnum * 64;
	for (unsigned int i = 0; i < header->e_shnum; i++) {
		elf_shdr_t sh;
		char name[128];
		err = section(&image, i, &sh);
		if (err || (err = name_at(&image, &names, sh.sh_name, name, sizeof(name)))) {
			return err;
		}
		if ((sh.sh_type != SHT_NOBITS && !range(sh.sh_offset, sh.sh_size, length)) ||
		    sh.sh_size > HEAP_LIMIT || sh.sh_addralign > HEAP_LIMIT ||
		    (sh.sh_addralign && !is_power_of_two(sh.sh_addralign)) ||
		    sh.sh_addr || (sh.sh_flags & 0xc00U) /* SHF_TLS | SHF_COMPRESSED */) {
			return -ENOEXEC;
		}
		if (!strcmp(name, ".brickphyr.abi")) {
			char identity[sizeof(BRICKPHYR_ABI)];
			if (abi || sh.sh_type != SHT_PROGBITS || sh.sh_size != sizeof(identity)) {
				return -ENOEXEC;
			}
			err = read_at(&image, sh.sh_offset, identity, sizeof(identity));
			if (err || memcmp(identity, BRICKPHYR_ABI, sizeof(identity))) {
				return err ? err : -EPROTONOSUPPORT;
			}
			abi = true;
		}
		if (sh.sh_flags & SHF_ALLOC) {
			if ((sh.sh_flags & (SHF_WRITE | SHF_EXECINSTR)) == (SHF_WRITE | SHF_EXECINSTR)) {
				return -ENOTSUP;
			}
			if (sh.sh_size) {
				size_t block = 1U << LOG2CEIL(MAX(32U, MAX(sh.sh_size, sh.sh_addralign)));
				budget += 2 * block;
			}
		} else {
			budget += sh.sh_size;
		}
		if (budget > HEAP_LIMIT) {
			return -ENOMEM;
		}
		if (sh.sh_type == SHT_SYMTAB) {
			elf_shdr_t strings;
			if (++symtabs != 1 || sh.sh_entsize != sizeof(elf_sym_t) ||
			    sh.sh_size % sizeof(elf_sym_t) ||
			    (err = section(&image, sh.sh_link, &strings))) {
				return -ENOEXEC;
			}
			if (strings.sh_type != SHT_STRTAB) {
				return -ENOEXEC;
			}
			budget += sh.sh_size;
			for (size_t offset = 0; offset < sh.sh_size; offset += sizeof(elf_sym_t)) {
				elf_sym_t sym;
				err = read_at(&image, sh.sh_offset + offset, &sym, sizeof(sym));
				if (err || (err = name_at(&image, &strings, sym.st_name, name, sizeof(name)))) {
					return err;
				}
				if (sym.st_shndx == SHN_UNDEF) {
					/* The SDK uses inline SVC stubs; no privileged imports. */
					if (offset || name[0]) {
						return -ENOTSUP;
					}
					continue;
				}
				if (sym.st_shndx == SHN_ABS && ELF_ST_TYPE(sym.st_info) == STT_FILE) {
					continue;
				}
				if (ELF_ST_TYPE(sym.st_info) == STT_FUNC && !(sym.st_value & 1U)) {
					return -ENOTSUP; /* Cortex-M executes Thumb code only. */
				}
				elf_shdr_t target;
				if (section(&image, sym.st_shndx, &target) ||
				    !range(sym.st_value & ~1U, sym.st_size, target.sh_size)) {
					return -ENOEXEC;
				}
				if (!strcmp(name, "app_main")) {
					if (entry || ELF_ST_TYPE(sym.st_info) != STT_FUNC ||
					    ELF_ST_BIND(sym.st_info) != STB_GLOBAL ||
					    !(target.sh_flags & SHF_EXECINSTR) || !(sym.st_value & 1)) {
						return -ENOEXEC;
					}
					entry = true;
				}
			}
		} else if (sh.sh_type == SHT_REL) {
			elf_shdr_t target, symbols;
			if (sh.sh_entsize != sizeof(elf_rel_t) || sh.sh_size % sizeof(elf_rel_t) ||
			    section(&image, sh.sh_info, &target) || section(&image, sh.sh_link, &symbols) ||
			    symbols.sh_type != SHT_SYMTAB || symbols.sh_entsize != sizeof(elf_sym_t)) {
				return -ENOEXEC;
			}
			for (size_t offset = 0; offset < sh.sh_size; offset += sizeof(elf_rel_t)) {
				elf_rel_t rel;
				err = read_at(&image, sh.sh_offset + offset, &rel, sizeof(rel));
				if (err || !range(rel.r_offset, sizeof(uint32_t), target.sh_size) ||
				    ELF_R_SYM(rel.r_info) >= symbols.sh_size / sizeof(elf_sym_t)) {
					return err ? err : -ENOEXEC;
				}
			}
		} else if (sh.sh_type == SHT_RELA || sh.sh_type == SHT_DYNSYM) {
			return -ENOTSUP;
		}
	}
	if (!abi || !entry || !symtabs) {
		return -ENOEXEC;
	}
	if (budget > HEAP_LIMIT) {
		return -ENOMEM;
	}
	*heap_size = budget;
	return 0;
}
