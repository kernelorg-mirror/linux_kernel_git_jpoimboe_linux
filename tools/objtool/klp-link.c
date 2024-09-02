// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024 Josh Poimboeuf <jpoimboe@kernel.org>
 */
#include <fcntl.h>
#include <gelf.h>
#include <objtool/objtool.h>
#include <objtool/warn.h>
#include <objtool/klp.h>
#include <linux/livepatch_ext.h>

/*
 * This runs on the livepatch module after all other linking has been done.  It
 * converts the intermediate __klp_relocs section into proper klp relocs to be
 * processed by livepatch.  This needs to run last to avoid linker wreckage.
 * Linkers don't tend to handle the "two rela sections for a single base
 * section" case very well.
 */
int cmd_klp_link(int argc, const char **argv)
{
	struct section *symtab, *klp_relocs;
	struct elf *elf;

	argc--;
	argv++;

	if (argc != 1) {
		fprintf(stderr, "%d\n", argc);
		fprintf(stderr, "usage: objtool link <file.ko>\n");
		return -1;
	}

	elf = elf_open_read(argv[0], O_RDWR);

	klp_relocs = find_section_by_name(elf, KLP_RELOCS_SEC);
	if (!klp_relocs)
		return 0;

	symtab = find_section_by_name(elf, ".symtab");
	if (!symtab)
		ERROR("missing .symtab");

	for (int i = 0; i < sec_size(klp_relocs) / sizeof(struct klp_reloc); i++) {
		struct klp_reloc *klp_reloc;
		unsigned long klp_reloc_off;
		struct section *sec, *tmp, *klp_rsec;
		unsigned long offset;
		struct reloc *reloc;
		char sym_modname[64];
		char rsec_name[SEC_NAME_LEN];
		u64 addend;
		struct symbol *sym, *klp_sym;

		klp_reloc_off = i * sizeof(*klp_reloc);
		klp_reloc = klp_relocs->data->d_buf + klp_reloc_off;

		/*
		 * Read __klp_relocs entry:
		 */

		/* klp_reloc.sec_offset */
		reloc = find_reloc_by_dest(elf, klp_relocs,
					   klp_reloc_off + offsetof(struct klp_reloc, offset));
		ERROR_ON(!reloc, "malformed " KLP_RELOCS_SEC " section");

		sec = reloc->sym->sec;
		offset = reloc_addend(reloc);

		/* klp_reloc.sym */
		reloc = find_reloc_by_dest(elf, klp_relocs,
					   klp_reloc_off + offsetof(struct klp_reloc, sym));
		ERROR_ON(!reloc, "malformed " KLP_RELOCS_SEC " section");

		klp_sym = reloc->sym;
		addend = reloc_addend(reloc);

		/* symbol format: .klp.sym.modname.sym_name,sympos */
		sscanf(klp_sym->name + strlen(KLP_SYM_PREFIX), "%55[^.]", sym_modname);

		/*
		 * Create klp reloc:
		 */

		/* section format: .klp.rela.sec_objname.section_name */
		snprintf(rsec_name, SEC_NAME_LEN, KLP_RELOC_SEC_PREFIX "%s.%s",
			 sym_modname, sec->name);
		klp_rsec = find_section_by_name(elf, rsec_name);

		if (!klp_rsec) {
			klp_rsec = elf_create_section(elf, rsec_name, 0,
						      elf_rela_size(elf),
						      SHT_RELA, elf_addr_size(elf),
						      SHF_ALLOC | SHF_INFO_LINK | SHF_RELA_LIVEPATCH);

			klp_rsec->sh.sh_link = symtab->idx;
			klp_rsec->sh.sh_info = sec->idx;
			klp_rsec->base = sec;
		}

		tmp = sec->rsec;
		sec->rsec = klp_rsec;
		elf_create_reloc(elf, sec, offset, klp_sym, addend, klp_reloc->type);
		sec->rsec = tmp;

		klp_sym->sym.st_shndx = SHN_LIVEPATCH;
		gelf_update_sym(symtab->data, klp_sym->idx, &klp_sym->sym);

		/*
		 * Disable original non-klp reloc by converting it to R_*_NONE:
		 */

		reloc = find_reloc_by_dest(elf, sec, offset);
		sym = reloc->sym;
		sym->sym.st_shndx = SHN_LIVEPATCH;
		set_reloc_type(elf, reloc, 0);
		gelf_update_sym(symtab->data, sym->idx, &sym->sym);
	}

	elf_write(elf);

	return 0;
}
