// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/static_call.h>

extern int my_func_add(int arg1, int arg2);
extern int my_func_sub(int arg1, int arg2);
DECLARE_STATIC_CALL(my_key, my_func_add);

static int cmdline_proc_show(struct seq_file *m, void *v)
{
	int ret;

	ret = static_call(my_key, 1, 2);
	printk("static call (orig): ret=%d\n", ret);

	static_call_update(my_key, my_func_sub);
	ret = static_call(my_key, 1, 2);
	printk("static call (sub): ret=%d\n", ret);

	static_call_update(my_key, my_func_add);
	ret = static_call(my_key, 1, 2);
	printk("static call (add): ret=%d\n", ret);

	seq_puts(m, saved_command_line);
	seq_putc(m, '\n');
	return 0;
}

static int __init proc_cmdline_init(void)
{
	proc_create_single("cmdline", 0, NULL, cmdline_proc_show);
	return 0;
}
fs_initcall(proc_cmdline_init);
