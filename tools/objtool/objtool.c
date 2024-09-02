// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015 Josh Poimboeuf <jpoimboe@redhat.com>
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <subcmd/exec-cmd.h>
#include <subcmd/pager.h>
#include <linux/kernel.h>

#include <objtool/builtin.h>
#include <objtool/objtool.h>
#include <objtool/warn.h>

bool help;

char *Objname;
static struct objtool_file file;

static void objtool_create_backup(const char *objname)
{
	int len = strlen(objname);
	char *buf, *base, *name = malloc(len+6);
	int s, d, l, t;

	name = malloc(len+6);
	ERROR_ON(!name, "malloc");

	strcpy(name, objname);
	strcpy(name + len, ".orig");

	d = open(name, O_CREAT|O_WRONLY|O_TRUNC, 0644);
	ERROR_ON(d < 0, "can't create '%s': %s", name, strerror(errno));

	s = open(objname, O_RDONLY);
	ERROR_ON(s < 0, "can't open '%s': %s", objname, strerror(errno));

	buf = malloc(4096);
	ERROR_ON(!buf, "malloc");

	while ((l = read(s, buf, 4096)) > 0) {
		base = buf;
		do {
			t = write(d, base, l);
			ERROR_ON(t < 0, "failed backup write");

			base += t;
			l -= t;
		} while (l);
	}

	ERROR_ON(l < 0, "failed backup read");

	free(name);
	free(buf);
	close(d);
	close(s);
}

struct objtool_file *objtool_open_read(const char *objname)
{
	if (Objname) {
		if (strcmp(Objname, objname))
			ERROR("won't handle more than one file at a time");

		return &file;
	}

	file.elf = elf_open_read(objname, O_RDWR);

	if (opts.backup)
		objtool_create_backup(objname);

	hash_init(file.insn_hash);
	INIT_LIST_HEAD(&file.retpoline_call_list);
	INIT_LIST_HEAD(&file.return_thunk_list);
	INIT_LIST_HEAD(&file.static_call_list);
	INIT_LIST_HEAD(&file.mcount_loc_list);
	INIT_LIST_HEAD(&file.endbr_list);
	INIT_LIST_HEAD(&file.call_list);
	file.ignore_unreachables = opts.no_unreachable;
	file.hints = false;

	return &file;
}

void objtool_pv_add(struct objtool_file *f, int idx, struct symbol *func)
{
	if (!opts.noinstr)
		return;

	if (!f->pv_ops)
		ERROR("paravirt confusion");

	/*
	 * These functions will be patched into native code,
	 * see paravirt_patch().
	 */
	if (!strcmp(func->name, "_paravirt_nop") ||
	    !strcmp(func->name, "_paravirt_ident_64"))
		return;

	/* already added this function */
	if (!list_empty(&func->pv_target))
		return;

	list_add(&func->pv_target, &f->pv_ops[idx].targets);
	f->pv_ops[idx].clean = false;
}

int main(int argc, const char **argv)
{
	static const char *UNUSED = "OBJTOOL_NOT_IMPLEMENTED";

	/* libsubcmd init */
	exec_cmd_init("objtool", UNUSED, UNUSED, UNUSED);
	pager_init(UNUSED);

	if (argc > 1 && !strcmp(argv[1], "klp")) {
		argc--;
		argv++;
		return cmd_klp(argc, argv);
	}

	return objtool_run(argc, argv);
}
