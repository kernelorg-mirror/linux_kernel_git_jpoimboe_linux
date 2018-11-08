/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STATIC_CALL_H
#define _LINUX_STATIC_CALL_H

/*
 * Static call support
 *
 * Static calls use code patching to hard-code function pointers into direct
 * branch instructions.  They give the flexibility of function pointers, but
 * with improved performance.  This is especially important for cases where
 * retpolines would otherwise be used, as retpolines can significantly impact
 * performance.
 *
 *
 * API overview:
 *
 *   DECLARE_STATIC_CALL(key, func);
 *   DEFINE_STATIC_CALL(key, func);
 *   static_call(key, args...);
 *   static_call_update(key, func);
 *
 *
 * Usage example:
 *
 *   # Start with the following functions (with identical prototypes):
 *   int func_a(int arg1, int arg2);
 *   int func_b(int arg1, int arg2);
 *
 *   # Define a 'my_key' reference, associated with func_a() by default
 *   DEFINE_STATIC_CALL(my_key, func_a);
 *
 *   # Call func_a()
 *   static_call(my_key, arg1, arg2);
 *
 *   # Update 'my_key' to point to func_b()
 *   static_call_update(my_key, func_b);
 *
 *   # Call func_b()
 *   static_call(my_key, arg1, arg2);
 *
 *
 * Implementation details:
 *
 *    This requires some arch-specific code (CONFIG_HAVE_STATIC_CALL).
 *    Otherwise basic indirect calls are used (with function pointers).
 *
 *    Each static_call() site calls into a trampoline associated with the key.
 *    The trampoline has a direct branch to the default function.  Updates to a
 *    key will modify the trampoline's branch destination.
 *
 *    If the arch has CONFIG_HAVE_STATIC_CALL_INLINE, then the call sites
 *    themselves will be patched at runtime to call the functions directly,
 *    rather than calling through the trampoline.  This requires objtool or a
 *    compiler plugin to detect all the static_call() sites and annotate them
 *    in the .static_call_sites section.
 */

#include <linux/types.h>
#include <linux/cpu.h>
#include <linux/static_call_types.h>

#ifdef CONFIG_HAVE_STATIC_CALL
#include <asm/static_call.h>
extern void arch_static_call_transform(void *site, void *tramp, void *func);
#endif


#define DECLARE_STATIC_CALL(key, func)					\
	extern struct static_call_key key;				\
	extern typeof(func) STATIC_CALL_TRAMP(key)


#ifdef CONFIG_HAVE_STATIC_CALL_INLINE

struct static_call_key {
	void *func, *tramp;
	/*
	 * List of modules (including vmlinux) and their call sites associated
	 * with this key.
	 */
	struct list_head site_mods;
};

struct static_call_mod {
	struct list_head list;
	struct module *mod; /* for vmlinux, mod == NULL */
	struct static_call_site *sites;
};

extern void __static_call_update(struct static_call_key *key, void *func);
extern int static_call_mod_init(struct module *mod);

#define DEFINE_STATIC_CALL(key, _func)					\
	DECLARE_STATIC_CALL(key, _func);				\
	struct static_call_key key = {					\
		.func = _func,						\
		.tramp = STATIC_CALL_TRAMP(key),			\
		.site_mods = LIST_HEAD_INIT(key.site_mods),		\
	};								\
	ARCH_DEFINE_STATIC_CALL_TRAMP(key, _func)

/*
 * __ADDRESSABLE() is used to ensure the key symbol doesn't get stripped from
 * the symbol table so objtool can reference it when it generates the
 * static_call_site structs.
 */
#define static_call(key, args...)					\
({									\
	__ADDRESSABLE(key);						\
	STATIC_CALL_TRAMP(key)(args);					\
})

#define static_call_update(key, func)					\
({									\
	BUILD_BUG_ON(!__same_type(func, STATIC_CALL_TRAMP(key)));	\
	__static_call_update(&key, func);				\
})

#define EXPORT_STATIC_CALL(key)						\
	EXPORT_SYMBOL(key);						\
	EXPORT_SYMBOL(STATIC_CALL_TRAMP(key))

#define EXPORT_STATIC_CALL_GPL(key)					\
	EXPORT_SYMBOL_GPL(key);						\
	EXPORT_SYMBOL_GPL(STATIC_CALL_TRAMP(key))


#elif defined(CONFIG_HAVE_STATIC_CALL)

struct static_call_key {
	void *func, *tramp;
};

#define DEFINE_STATIC_CALL(key, _func)					\
	DECLARE_STATIC_CALL(key, _func);				\
	struct static_call_key key = {					\
		.func = _func,						\
		.tramp = STATIC_CALL_TRAMP(key),			\
	};								\
	ARCH_DEFINE_STATIC_CALL_TRAMP(key, _func)

#define static_call(key, args...) STATIC_CALL_TRAMP(key)(args)

static inline void __static_call_update(struct static_call_key *key, void *func)
{
	cpus_read_lock();
	WRITE_ONCE(key->func, func);
	arch_static_call_transform(NULL, key->tramp, func);
	cpus_read_unlock();
}

#define static_call_update(key, func)					\
({									\
	BUILD_BUG_ON(!__same_type(func, STATIC_CALL_TRAMP(key)));	\
	__static_call_update(&key, func);				\
})

#define EXPORT_STATIC_CALL(key)						\
	EXPORT_SYMBOL(STATIC_CALL_TRAMP(key))

#define EXPORT_STATIC_CALL_GPL(key)					\
	EXPORT_SYMBOL_GPL(STATIC_CALL_TRAMP(key))


#else /* Generic implementation */

struct static_call_key {
	void *func;
};

#define DEFINE_STATIC_CALL(key, _func)					\
	DECLARE_STATIC_CALL(key, _func);				\
	struct static_call_key key = {					\
		.func = _func,						\
	}

#define static_call(key, args...)					\
	((typeof(STATIC_CALL_TRAMP(key))*)(key.func))(args)

static inline void __static_call_update(struct static_call_key *key, void *func)
{
	WRITE_ONCE(key->func, func);
}

#define static_call_update(key, func)					\
({									\
	BUILD_BUG_ON(!__same_type(func, STATIC_CALL_TRAMP(key)));	\
	__static_call_update(&key, func);				\
})

#define EXPORT_STATIC_CALL(key) EXPORT_SYMBOL(key)
#define EXPORT_STATIC_CALL_GPL(key) EXPORT_SYMBOL_GPL(key)

#endif /* CONFIG_HAVE_STATIC_CALL_INLINE */

#endif /* _LINUX_STATIC_CALL_H */
