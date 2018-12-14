// SPDX-License-Identifier: GPL-2.0
/*
 * Procfs support for lockd
 *
 * Copyright (c) 2014 Jeff Layton <jlayton@primarydata.com>
 */

#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/nsproxy.h>
#include <net/net_namespace.h>

#include "netns.h"
#include "procfs.h"

/*
 * We only allow strings that start with 'Y', 'y', or '1'.
 */
static ssize_t
nlm_end_grace_write(struct file *file, const char __user *buf, size_t size,
		    loff_t *pos)
{
	char *data;
	struct lockd_net *ln = net_generic(current->nsproxy->net_ns,
					   lockd_net_id);

	if (size < 1)
		return -EINVAL;

	data = simple_transaction_get(file, buf, size);
	if (IS_ERR(data))
		return PTR_ERR(data);

	switch(data[0]) {
	case 'Y':
	case 'y':
	case '1':
		locks_end_grace(&ln->lockd_manager);
		break;
	default:
		return -EINVAL;
	}

	return size;
}

#include <linux/static_call.h>
extern int my_func_add(int arg1, int arg2);
extern int my_func_sub(int arg1, int arg2);
DECLARE_STATIC_CALL(my_mod_key, my_func_add);

DEFINE_STATIC_CALL(my_mod_key, my_func_sub);
EXPORT_STATIC_CALL_GPL(my_mod_key);

static ssize_t
nlm_end_grace_read(struct file *file, char __user *buf, size_t size,
		   loff_t *pos)
{
	struct lockd_net *ln = net_generic(current->nsproxy->net_ns,
					   lockd_net_id);
	char resp[5];
	static int call;

	resp[0] = list_empty(&ln->lockd_manager.list) ? 'Y' : 'N';
	resp[1] = '\n';
	if (call++ % 2 == 0) {
		if (static_call(my_mod_key, 1, 2) == -1) {
			resp[2] = '-';
			printk("add\n");
			static_call_update(my_mod_key, my_func_add);
		} else {
			resp[2] = '+';
			printk("sub\n");
			static_call_update(my_mod_key, my_func_sub);
		}
	}
	resp[3] = '\n';
	resp[4] = '\0';

	return simple_read_from_buffer(buf, size, pos, resp, sizeof(resp));
}

static const struct file_operations lockd_end_grace_operations = {
	.write		= nlm_end_grace_write,
	.read		= nlm_end_grace_read,
	.llseek		= default_llseek,
	.release	= simple_transaction_release,
};

#include <linux/static_call.h>
extern int my_func_add(int arg1, int arg2);
DECLARE_STATIC_CALL(my_mod_key, my_func_add);

int __init
lockd_create_procfs(void)
{
	struct proc_dir_entry *entry;

	entry = proc_mkdir("fs/lockd", NULL);
	if (!entry)
		return -ENOMEM;
	entry = proc_create("nlm_end_grace", S_IRUGO|S_IWUSR, entry,
				 &lockd_end_grace_operations);
	if (!entry) {
		remove_proc_entry("fs/lockd", NULL);
		return -ENOMEM;
	}

	printk("lockd static call: %d\n", static_call(my_mod_key, 1, 2));

	return 0;
}

void __exit
lockd_remove_procfs(void)
{
	remove_proc_entry("fs/lockd/nlm_end_grace", NULL);
	remove_proc_entry("fs/lockd", NULL);
}
