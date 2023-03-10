/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STATIC_CALL_H
#define _LINUX_STATIC_CALL_H

/*
 * Static call support
 *
 * Static calls use code patching to hard-code function pointers into direct
 * branch instructions. They give the flexibility of function pointers, but
 * with improved performance. This is especially important for cases where
 * retpolines would otherwise be used, as retpolines can significantly impact
 * performance.
 *
 *
 * API overview:
 *
 *   DECLARE_STATIC_CALL(name, func);
 *   DEFINE_STATIC_CALL(name, func);
 *   DEFINE_STATIC_CALL_NULL(name, typename);
 *   DEFINE_STATIC_CALL_RET0(name, typename);
 *
 *   __static_call_return0;
 *
 *   static_call(name)(args...);
 *   static_call_cond(name)(args...);
 *   static_call_update(name, func);
 *   static_call_query(name);
 *
 *   EXPORT_STATIC_CALL{,_TRAMP}{,_GPL}()
 *
 * Usage example:
 *
 *   # Start with the following functions (with identical prototypes):
 *   int func_a(int arg1, int arg2);
 *   int func_b(int arg1, int arg2);
 *
 *   # Define a 'my_name' reference, associated with func_a() by default
 *   DEFINE_STATIC_CALL(my_name, func_a);
 *
 *   # Call func_a()
 *   static_call(my_name)(arg1, arg2);
 *
 *   # Update 'my_name' to point to func_b()
 *   static_call_update(my_name, &func_b);
 *
 *   # Call func_b()
 *   static_call(my_name)(arg1, arg2);
 *
 *
 * Implementation details:
 *
 *   This requires some arch-specific code (CONFIG_HAVE_STATIC_CALL).
 *   Otherwise basic indirect calls are used (with function pointers).
 *
 *   Each static_call() site calls into a trampoline associated with the name.
 *   The trampoline has a direct branch to the default function.  Updates to a
 *   name will modify the trampoline's branch destination.
 *
 *   If the arch has CONFIG_HAVE_STATIC_CALL_INLINE, then the call sites
 *   themselves will be patched at runtime to call the functions directly,
 *   rather than calling through the trampoline.  This requires objtool or a
 *   compiler plugin to detect all the static_call() sites and annotate them
 *   in the .static_call_sites section.
 *
 *
 * Notes on NULL function pointers:
 *
 *   A static_call() to a NULL function pointer is equivalent to a call to a
 *   "do nothing return 0" function.
 *
 *   A NULL static call can be the result of:
 *
 *     DECLARE_STATIC_CALL_NULL(my_static_call, void (*)(int));
 *
 *   or using static_call_update() with a NULL function.
 *
 *   The "return 0" feature is strictly UB per the C standard (since it casts a
 *   function pointer to a different signature) and relies on the architecture
 *   ABI to make things work. In particular it relies on the return value
 *   register being callee-clobbered for all function calls.
 *
 *   In particular The x86_64 implementation of HAVE_STATIC_CALL_INLINE
 *   replaces the 5 byte CALL instruction at the callsite with a 5 byte clear
 *   of the RAX register, completely eliding any function call overhead.
 *
 *   Any argument evaluation is unconditional. Unlike a regular conditional
 *   function pointer call:
 *
 *     if (my_func_ptr)
 *         my_func_ptr(arg1)
 *
 *   where the argument evaluation also depends on the pointer value.
 *
 *
 * EXPORT_STATIC_CALL() vs EXPORT_STATIC_CALL_TRAMP():
 *
 *   The difference is that the _TRAMP variant tries to only export the
 *   trampoline with the result that a module can use static_call{,_cond}() but
 *   not static_call_update().
 *
 */

#include <linux/types.h>
#include <linux/cpu.h>
#include <linux/static_call_types.h>

#ifdef CONFIG_HAVE_STATIC_CALL
#include <asm/static_call.h>

/*
 * Either @site or @tramp can be NULL.
 */
extern void arch_static_call_transform(void *site, void *tramp, void *func, bool tail);

#define STATIC_CALL_TRAMP_ADDR(name) &STATIC_CALL_TRAMP(name)

#else
#define STATIC_CALL_TRAMP_ADDR(name) NULL
#endif

extern long __static_call_return0(void);

#define static_call_update(name, func)					\
({									\
	typeof(&STATIC_CALL_TRAMP(name)) __F = (func);			\
	void *__f = (void *)__F ? : (void *)__static_call_return0;	\
	__static_call_update(&STATIC_CALL_KEY(name),			\
			     STATIC_CALL_TRAMP_ADDR(name), __f);	\
})

#define static_call_query(name)						\
({									\
	void *__func = (READ_ONCE(STATIC_CALL_KEY(name).func));		\
	__func == __static_call_return0 ? NULL : __func;		\
})

#ifdef CONFIG_HAVE_STATIC_CALL_INLINE

extern int __init static_call_init(void);

extern void static_call_force_reinit(void);

struct static_call_mod {
	struct static_call_mod *next;
	struct module *mod; /* for vmlinux, mod == NULL */
	struct static_call_site *sites;
};

/* For finding the key associated with a trampoline */
struct static_call_tramp_key {
	s32 tramp;
	s32 key;
};

extern void __static_call_update(struct static_call_key *key, void *tramp, void *func);
extern int static_call_mod_init(struct module *mod);
extern int static_call_text_reserved(void *start, void *end);

#define DEFINE_STATIC_CALL(name, _func)					\
	DECLARE_STATIC_CALL(name, _func);				\
	struct static_call_key STATIC_CALL_KEY(name) = {		\
		.func = _func,						\
		.type = 1,						\
	};								\
	ARCH_DEFINE_STATIC_CALL_TRAMP(name, _func)

#define DEFINE_STATIC_CALL_NULL(name, _func)				\
	DECLARE_STATIC_CALL(name, _func);				\
	struct static_call_key STATIC_CALL_KEY(name) = {		\
		.func = __static_call_return0,				\
		.type = 1,						\
	};								\
	ARCH_DEFINE_STATIC_CALL_RET0_TRAMP(name)

#define DEFINE_STATIC_CALL_RET0 DEFINE_STATIC_CALL_NULL

#define static_call_cond(name)	__static_call(name)

#define EXPORT_STATIC_CALL(name)					\
	EXPORT_SYMBOL(STATIC_CALL_KEY(name));				\
	EXPORT_SYMBOL(STATIC_CALL_TRAMP(name))
#define EXPORT_STATIC_CALL_GPL(name)					\
	EXPORT_SYMBOL_GPL(STATIC_CALL_KEY(name));			\
	EXPORT_SYMBOL_GPL(STATIC_CALL_TRAMP(name))

/* Leave the key unexported, so modules can't change static call targets: */
#define EXPORT_STATIC_CALL_TRAMP(name)					\
	EXPORT_SYMBOL(STATIC_CALL_TRAMP(name));				\
	ARCH_ADD_TRAMP_KEY(name)
#define EXPORT_STATIC_CALL_TRAMP_GPL(name)				\
	EXPORT_SYMBOL_GPL(STATIC_CALL_TRAMP(name));			\
	ARCH_ADD_TRAMP_KEY(name)

#elif defined(CONFIG_HAVE_STATIC_CALL)

static inline int static_call_init(void) { return 0; }

#define DEFINE_STATIC_CALL(name, _func)					\
	DECLARE_STATIC_CALL(name, _func);				\
	struct static_call_key STATIC_CALL_KEY(name) = {		\
		.func = _func,						\
	};								\
	ARCH_DEFINE_STATIC_CALL_TRAMP(name, _func)

#define DEFINE_STATIC_CALL_NULL(name, _func)				\
	DECLARE_STATIC_CALL(name, _func);				\
	struct static_call_key STATIC_CALL_KEY(name) = {		\
		.func = __static_call_return0,				\
	};								\
	ARCH_DEFINE_STATIC_CALL_NULL_TRAMP(name)

#define DEFINE_STATIC_CALL_RET0 DEFINE_STATIC_CALL_NULL

#define static_call_cond(name)	__static_call(name)

static inline
void __static_call_update(struct static_call_key *key, void *tramp, void *func)
{
	cpus_read_lock();
	WRITE_ONCE(key->func, func);
	arch_static_call_transform(NULL, tramp, func, false);
	cpus_read_unlock();
}

static inline int static_call_text_reserved(void *start, void *end)
{
	return 0;
}

#define EXPORT_STATIC_CALL(name)					\
	EXPORT_SYMBOL(STATIC_CALL_KEY(name));				\
	EXPORT_SYMBOL(STATIC_CALL_TRAMP(name))
#define EXPORT_STATIC_CALL_GPL(name)					\
	EXPORT_SYMBOL_GPL(STATIC_CALL_KEY(name));			\
	EXPORT_SYMBOL_GPL(STATIC_CALL_TRAMP(name))

/* Leave the key unexported, so modules can't change static call targets: */
#define EXPORT_STATIC_CALL_TRAMP(name)					\
	EXPORT_SYMBOL(STATIC_CALL_TRAMP(name))
#define EXPORT_STATIC_CALL_TRAMP_GPL(name)				\
	EXPORT_SYMBOL_GPL(STATIC_CALL_TRAMP(name))

#else /* !CONFIG_HAVE_STATIC_CALL */

static inline int static_call_init(void) { return 0; }

#define __DEFINE_STATIC_CALL(name, _func, _func_init)			\
	DECLARE_STATIC_CALL(name, _func);				\
	struct static_call_key STATIC_CALL_KEY(name) = {		\
		.func = _func_init,					\
	}

#define DEFINE_STATIC_CALL(name, _func)					\
	__DEFINE_STATIC_CALL(name, _func, _func)

#define DEFINE_STATIC_CALL_NULL(name, _func)				\
	__DEFINE_STATIC_CALL(name, _func, __static_call_return0)

#define DEFINE_STATIC_CALL_RET0 DEFINE_STATIC_CALL_NULL

#define static_call_cond(name) static_call(name)

static inline
void __static_call_update(struct static_call_key *key, void *tramp, void *func)
{
	WRITE_ONCE(key->func, func);
}

static inline int static_call_text_reserved(void *start, void *end)
{
	return 0;
}

#define EXPORT_STATIC_CALL(name)	EXPORT_SYMBOL(STATIC_CALL_KEY(name))
#define EXPORT_STATIC_CALL_GPL(name)	EXPORT_SYMBOL_GPL(STATIC_CALL_KEY(name))

#endif /* CONFIG_HAVE_STATIC_CALL */

#endif /* _LINUX_STATIC_CALL_H */
