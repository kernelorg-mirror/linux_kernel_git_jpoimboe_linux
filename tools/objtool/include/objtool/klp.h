/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 Josh Poimboeuf <jpoimboe@kernel.org>
 */
#ifndef _OBJTOOL_KLP_H
#define _OBJTOOL_KLP_H

#define SHF_RELA_LIVEPATCH	0x00100000
#define SHN_LIVEPATCH		0xff20

#define KLP_RELOCS_SEC	".klp.relocs"
#define KLP_OBJECTS_SEC	"__klp_objects"
#define KLP_FUNCS_SEC	"__klp_funcs"
#define KLP_STRINGS_SEC	".rodata.klp.str1.1"

struct klp_reloc {
	void *offset;
	void *sym;
	u32 type;
};

int cmd_klp_diff(int argc, const char **argv);
int cmd_klp_link(int argc, const char **argv);

#endif /* _OBJTOOL_KLP_H */
