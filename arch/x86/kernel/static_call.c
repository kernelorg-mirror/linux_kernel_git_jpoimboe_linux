/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/init.h>
#include <linux/static_call.h>
#include <linux/bug.h>
#include <linux/smp.h>
#include <linux/memory.h>
#include <linux/sort.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <asm/text-patching.h>
#include <asm/processor.h>
#include <asm/sections.h>

extern int cmdline_proc_show(void);

extern struct static_call_site __start_static_call_sites[],
			       __stop_static_call_sites[];

static bool static_call_initialized;

/* mutex to protect key modules/sites */
static DEFINE_MUTEX(static_call_mutex);

static void static_call_lock(void)
{
	mutex_lock(&static_call_mutex);
}

static void static_call_unlock(void)
{
	mutex_unlock(&static_call_mutex);
}

static inline unsigned long static_call_addr(struct static_call_site *site)
{
	return (long)site->addr + (long)&site->addr;
}

static inline struct static_call_key *static_call_key(const struct static_call_site *site)
{
	return (struct static_call_key *)((long)site->key + (long)&site->key);
}

static int static_call_site_cmp(const void *_a, const void *_b)
{
	const struct static_call_site *a = _a;
	const struct static_call_site *b = _b;
	const struct static_call_key *key_a = static_call_key(a);
	const struct static_call_key *key_b = static_call_key(b);

	if (key_a < key_b)
		return -1;

	if (key_a > key_b)
		return 1;

	return 0;
}

static void static_call_site_swap(void *_a, void *_b, int size)
{
	long delta = (unsigned long)_a - (unsigned long)_b;
	struct static_call_site *a = _a;
	struct static_call_site *b = _b;
	struct static_call_site tmp = *a;

	a->addr = b->addr  - delta;
	a->key  = b->key   - delta;

	b->addr = tmp.addr + delta;
	b->key  = tmp.key  + delta;
}

static inline void static_call_sort_entries(struct static_call_site *start,
					    struct static_call_site *stop)
{
	sort(start, stop - start, sizeof(struct static_call_site),
	     static_call_site_cmp, static_call_site_swap);
}


void static_call_bp_handler(void);
void *bp_handler_func;
void *bp_handler_continue;

asm(".pushsection .text, \"ax\"						\n"
    ".globl static_call_bp_handler					\n"
    ".type static_call_bp_handler, @function				\n"
    "static_call_bp_handler:						\n"
    "ANNOTATE_RETPOLINE_SAFE						\n"
    "call *bp_handler_func(%rip)					\n"
    "ANNOTATE_RETPOLINE_SAFE						\n"
    "jmp *bp_handler_continue(%rip)					\n"
    ".popsection							\n");

#define CALL_INSN_SIZE 5

static void static_call_transform(struct static_call_site *site, bool early)
{
	s32 call_dest;
	unsigned char insn_opcode;
	unsigned long insn = static_call_addr(site);
	struct static_call_key *key = static_call_key(site);
	void *func = key->func;
	unsigned char opcodes[CALL_INSN_SIZE];

	insn_opcode = *(unsigned char *)insn;
	if (insn_opcode != 0xe8 && insn_opcode != 0xe9) {
		WARN_ONCE(1, "unexpected static call insn opcode %x at %pS",
			  insn_opcode, (void *)insn);
		return;
	}

	call_dest = (long)(func) - (long)(insn + CALL_INSN_SIZE);

	opcodes[0] = insn_opcode;
	memcpy(&opcodes[1], &call_dest, CALL_INSN_SIZE - 1);

	if (early) {
		text_poke_early((void *)insn+1, &opcodes[1],
				CALL_INSN_SIZE - 1);
	} else {

		mutex_lock(&text_mutex);

		/* Set up the variables for the breakpoint handler: */
		bp_handler_func = func;
		bp_handler_continue = (void *)(insn + CALL_INSN_SIZE);

		/* Patch the call site: */
		text_poke_bp((void *)insn, opcodes, CALL_INSN_SIZE,
			     static_call_bp_handler);

		mutex_unlock(&text_mutex);
	}
}

void __static_call_update(struct static_call_key *key, void *func)
{
	struct static_call_mod *mod;
	struct static_call_site *site, *stop;
	bool early = false;

	cpus_read_lock();
	static_call_lock();

	if (key->func == func)
		goto done;

	key->func = func;

	/*
	 * If called before init, leave the call sites unpatched for now.
	 * They'll make direct calls to the trampoline until static_call_init()
	 * is called.
	 */
	if (!static_call_initialized)
		goto done;

	list_for_each_entry(mod, &key->modules, list) {
		if (!mod->sites) {
			/*
			 * This can happen if the static call key is defined in
			 * a module which doesn't use it.
			 */
			continue;
		}

		if (!mod->mod) {
			/* vmlinux */
			early = system_state < SYSTEM_RUNNING;
			stop = __stop_static_call_sites;
		} else {
#ifdef CONFIG_MODULES
			/* module */
			early = mod->mod->state == MODULE_STATE_COMING;
			stop = mod->mod->arch.static_call_sites +
			       mod->mod->arch.num_static_call_sites;
#endif
		}

		for (site = mod->sites;
		     site < stop && static_call_key(site) == key; site++) {
			unsigned long addr = static_call_addr(site);

			if (!mod->mod && init_section_contains((void *)addr, 1))
				continue;
			if (mod->mod && within_module_init(addr, mod->mod))
				continue;

			static_call_transform(site, early);
		}
	}

done:
	static_call_unlock();
	cpus_read_unlock();
}

#ifdef CONFIG_MODULES

static int static_call_add_module(struct module *mod)
{
	struct static_call_site *start = mod->arch.static_call_sites;
	struct static_call_site *stop = mod->arch.static_call_sites +
					mod->arch.num_static_call_sites;
	struct static_call_site *site;
	struct static_call_key *key, *prev_key = NULL;
	struct static_call_mod *static_call_mod;

	if (start == stop)
		return 0;

	static_call_sort_entries(start, stop);

	for (site = start; site < stop; site++) {
		if (within_module_init(static_call_addr(site), mod))
			continue;

		key = static_call_key(site);
		if (key == prev_key)
			continue;
		prev_key = key;

		static_call_mod = kzalloc(sizeof(*static_call_mod), GFP_KERNEL);
		if (!static_call_mod)
			return -ENOMEM;

		static_call_mod->mod = mod;
		static_call_mod->sites = site;
		list_add_tail(&static_call_mod->list, &key->modules);
	}

	return 0;
}

static void static_call_del_module(struct module *mod)
{
	struct static_call_site *start = mod->arch.static_call_sites;
	struct static_call_site *stop = mod->arch.static_call_sites +
					mod->arch.num_static_call_sites;
	struct static_call_site *site;
	struct static_call_key *key, *prev_key = NULL;
	struct static_call_mod *static_call_mod;

	for (site = start; site < stop; site++) {
		key = static_call_key(site);
		if (key == prev_key)
			continue;
		prev_key = key;

		list_for_each_entry(static_call_mod, &key->modules, list) {
			if (static_call_mod->mod == mod) {
				list_del(&static_call_mod->list);
				kfree(static_call_mod);
				break;
			}
		}
	}
}

static int static_call_module_notify(struct notifier_block *nb,
				     unsigned long val, void *data)
{
	struct module *mod = data;
	int ret = 0;

	cpus_read_lock();
	static_call_lock();

	switch (val) {
	case MODULE_STATE_COMING:
		ret = static_call_add_module(mod);
		if (ret) {
			WARN(1, "Failed to allocate memory for static calls");
			static_call_del_module(mod);
		}
		break;
	case MODULE_STATE_GOING:
		static_call_del_module(mod);
		break;
	}

	static_call_unlock();
	cpus_read_unlock();

	return notifier_from_errno(ret);
}

static struct notifier_block static_call_module_nb = {
	.notifier_call = static_call_module_notify,
};

#endif /* CONFIG_MODULES */

void __init static_call_init(void)
{
	struct static_call_site *start = __start_static_call_sites;
	struct static_call_site *stop  = __stop_static_call_sites;
	struct static_call_site *site;

	if (start == stop) {
		pr_warn("WARNING: empty static call table\n");
		return;
	}

	cpus_read_lock();
	static_call_lock();

	static_call_sort_entries(start, stop);

	for (site = start; site < stop; site++) {
		struct static_call_key *key = static_call_key(site);

		if (list_empty(&key->modules)) {
			struct static_call_mod *mod;

			mod = kzalloc(sizeof(*mod), GFP_KERNEL);
			if (!mod) {
				WARN(1, "Failed to allocate memory for static calls");
				return;
			}

			mod->sites = site;
			list_add_tail(&mod->list, &key->modules);
		}

		if (!init_section_contains((void *)static_call_addr(site), 1))
			static_call_transform(site, true);
	}

#ifdef CONFIG_MODULES
	register_module_notifier(&static_call_module_nb);
#endif

	static_call_initialized = true;

	static_call_unlock();
	cpus_read_unlock();
}
early_initcall(static_call_init);

/*** TEST CODE BELOW -- called from cmdline_proc_show ***/

int my_func_add(int arg1, int arg2)
{
	return arg1 + arg2;
}

int my_func_sub(int arg1, int arg2)
{
	return arg1 - arg2;
}

DEFINE_STATIC_CALL(my_key, my_func_add);
STATIC_CALL_EXPORT(my_key);
