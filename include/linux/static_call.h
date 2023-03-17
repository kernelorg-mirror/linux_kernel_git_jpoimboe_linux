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
 *   static_call_ro(name)(args...);
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
 *   Static_call()s support NULL functions, with many of the caveats that
 *   regular function pointers have.
 *
 *   Clearly calling a NULL function pointer is 'BAD', so too for
 *   static_call()s (although when HAVE_STATIC_CALL it might not be immediately
 *   fatal). A NULL static_call can be the result of:
 *
 *     DECLARE_STATIC_CALL_NULL(my_static_call, void (*)(int));
 *
 *   which is equivalent to declaring a NULL function pointer with just a
 *   typename:
 *
 *     void (*my_func_ptr)(int arg1) = NULL;
 *
 *   or using static_call_update() with a NULL function. In both cases the
 *   HAVE_STATIC_CALL implementation will patch the trampoline with a RET
 *   instruction, instead of an immediate tail-call JMP. HAVE_STATIC_CALL_INLINE
 *   architectures can patch the trampoline call to a NOP.
 *
 *   In all cases, any argument evaluation is unconditional. Unlike a regular
 *   conditional function pointer call:
 *
 *     if (my_func_ptr)
 *         my_func_ptr(arg1)
 *
 *   where the argument evaludation also depends on the pointer value.
 *
 *   When calling a static_call that can be NULL, use:
 *
 *     static_call_cond(name)(arg1);
 *
 *   which will include the required value tests to avoid NULL-pointer
 *   dereferences.
 *
 *   To query which function is currently set to be called, use:
 *
 *   func = static_call_query(name);
 *
 *
 * DEFINE_STATIC_CALL_RET0 / __static_call_return0:
 *
 *   Just like how DEFINE_STATIC_CALL_NULL() / static_call_cond() optimize the
 *   conditional void function call, DEFINE_STATIC_CALL_RET0 /
 *   __static_call_return0 optimize the do nothing return 0 function.
 *
 *   This feature is strictly UB per the C standard (since it casts a function
 *   pointer to a different signature) and relies on the architecture ABI to
 *   make things work. In particular it relies on Caller Stack-cleanup and the
 *   whole return register being clobbered for short return values. All normal
 *   CDECL style ABIs conform.
 *
 *   In particular the x86_64 implementation replaces the 5 byte CALL
 *   instruction at the callsite with a 5 byte clear of the RAX register,
 *   completely eliding any function call overhead.
 *
 *   Notably argument setup is unconditional.
 *
 *
 * EXPORT_STATIC_CALL() vs EXPORT_STATIC_CALL_RO():
 *
 *   The difference is the read-only variant exports the trampoline but not the
 *   key, so a module can call it via static_call_ro() but can't update the
 *   target via static_call_update().
 */

#include <linux/types.h>
#include <linux/static_call_types.h>

struct static_call_mods;
struct static_call_key {
	void *func;
#ifdef CONFIG_HAVE_STATIC_CALL_INLINE
	union {
		/* bit 0: 0 = sites, 1 = mods */
		unsigned long type;
		struct static_call_site *_sites;
		struct static_call_mod *_mods;
	};
#endif
};

#define DECLARE_STATIC_CALL(name, func)					\
	extern struct static_call_key STATIC_CALL_KEY(name);		\
	extern typeof(func) STATIC_CALL_TRAMP(name);

#define __DEFINE_STATIC_CALL(name, type, _func)				\
	DECLARE_STATIC_CALL(name, type);				\
	struct static_call_key STATIC_CALL_KEY(name) = {		\
		.func = _func,						\
	}

#define DEFINE_STATIC_CALL(name, func)					\
	__DEFINE_STATIC_CALL(name, func, func);				\
	__DEFINE_STATIC_CALL_TRAMP(name, func)

#define DEFINE_STATIC_CALL_NULL(name, type)				\
	__DEFINE_STATIC_CALL(name, type, NULL);				\
	__DEFINE_STATIC_CALL_NULL_TRAMP(name)

#define DEFINE_STATIC_CALL_RET0(name, type)				\
	__DEFINE_STATIC_CALL(name, type, __static_call_return0);	\
	__DEFINE_STATIC_CALL_RET0_TRAMP(name)

#define EXPORT_STATIC_CALL(name)					\
	EXPORT_SYMBOL(STATIC_CALL_KEY(name));				\
	__EXPORT_STATIC_CALL_TRAMP(name)
#define EXPORT_STATIC_CALL_GPL(name)					\
	EXPORT_SYMBOL_GPL(STATIC_CALL_KEY(name));			\
	__EXPORT_STATIC_CALL_TRAMP_GPL(name)

/*
 * Read-only exports: export the trampoline but not the key, so modules can't
 * change call targets.
 *
 * These are called via static_call_ro().
 */
#define EXPORT_STATIC_CALL_RO(name)					\
	__EXPORT_STATIC_CALL_TRAMP(name);				\
	__STATIC_CALL_ADD_TRAMP_KEY(name)
#define EXPORT_STATIC_CALL_RO_GPL(name)					\
	__EXPORT_STATIC_CALL_TRAMP_GPL(name);				\
	__STATIC_CALL_ADD_TRAMP_KEY(name)

/*
 * __ADDRESSABLE() is used to ensure the key symbol doesn't get stripped from
 * the symbol table so that objtool can reference it when it generates the
 * .static_call_sites section.
 */
#define __STATIC_CALL_ADDRESSABLE(name) __ADDRESSABLE(STATIC_CALL_KEY(name))

#define static_call(name)						\
({									\
	__STATIC_CALL_ADDRESSABLE(name);				\
	__static_call(name);						\
})

#define static_call_cond(name)		(void)__static_call_cond(name)

/* Use static_call_ro() to call a read-only-exported static call. */
#define static_call_ro(name)		__static_call_ro(name)

#if defined(MODULE) || !defined(CONFIG_HAVE_STATIC_CALL_INLINE)
#define __STATIC_CALL_RO_ADDRESSABLE(name)
#define __static_call_ro(name)		__static_call(name)
#else
#define __STATIC_CALL_RO_ADDRESSABLE(name) __STATIC_CALL_ADDRESSABLE(name)
#define __static_call_ro(name)		static_call(name)
#endif

#define static_call_update(name, func)					\
({									\
	typeof(&STATIC_CALL_TRAMP(name)) __F = (func);			\
	__static_call_update(&STATIC_CALL_KEY(name),			\
			     STATIC_CALL_TRAMP_ADDR(name), __F);	\
})

#define static_call_query(name) (READ_ONCE(STATIC_CALL_KEY(name).func))


#ifdef CONFIG_HAVE_STATIC_CALL

#include <asm/static_call.h>

#define __DEFINE_STATIC_CALL_TRAMP(name, func)				\
	ARCH_DEFINE_STATIC_CALL_TRAMP(name, func)

#define __DEFINE_STATIC_CALL_NULL_TRAMP(name)				\
	ARCH_DEFINE_STATIC_CALL_NULL_TRAMP(name)

#define __DEFINE_STATIC_CALL_RET0_TRAMP(name)				\
	ARCH_DEFINE_STATIC_CALL_RET0_TRAMP(name)

#define __EXPORT_STATIC_CALL_TRAMP(name)				\
	EXPORT_SYMBOL(STATIC_CALL_TRAMP(name))

#define __EXPORT_STATIC_CALL_TRAMP_GPL(name)				\
	EXPORT_SYMBOL_GPL(STATIC_CALL_TRAMP(name))

#define __static_call(name)		(&STATIC_CALL_TRAMP(name))
#define __static_call_cond		__static_call

#define STATIC_CALL_TRAMP_ADDR(name)	&STATIC_CALL_TRAMP(name)

extern long __static_call_return0(void);
extern void __static_call_update(struct static_call_key *key, void *tramp, void *func);

/*
 * Either @site or @tramp can be NULL.
 */
extern void arch_static_call_transform(void *site, void *tramp, void *func, bool tail);

#else /* !CONFIG_HAVE_STATIC_CALL */

#define __DEFINE_STATIC_CALL_TRAMP(name, func)
#define __DEFINE_STATIC_CALL_NULL_TRAMP(name)
#define __DEFINE_STATIC_CALL_RET0_TRAMP(name)
#define __EXPORT_STATIC_CALL_TRAMP(name)
#define __EXPORT_STATIC_CALL_TRAMP_GPL(name)

#define __static_call(name)						\
	((typeof(STATIC_CALL_TRAMP(name))*)(STATIC_CALL_KEY(name).func))

static inline void __static_call_nop(void) { }
/*
 * This horrific hack takes care of two things:
 *
 *  - it ensures the compiler will only load the function pointer ONCE,
 *    which avoids a reload race.
 *
 *  - it ensures the argument evaluation is unconditional, similar
 *    to the HAVE_STATIC_CALL variant.
 *
 * Sadly current GCC/Clang (10 for both) do not optimize this properly
 * and will emit an indirect call for the NULL case :-(
 */
#define __static_call_cond(name)					\
({									\
	void *func = READ_ONCE(STATIC_CALL_KEY(name).func);		\
	if (!func)							\
		func = &__static_call_nop;				\
	(typeof(STATIC_CALL_TRAMP(name))*)func;				\
})

#define STATIC_CALL_TRAMP_ADDR(name)	NULL

static inline long __static_call_return0(void) { return 0; }

static inline
void __static_call_update(struct static_call_key *key, void *tramp, void *func)
{
	WRITE_ONCE(key->func, func);
}

#endif /* CONFIG_HAVE_STATIC_CALL */


#ifdef CONFIG_HAVE_STATIC_CALL_INLINE

/* Unexported key lookup table */
#define __STATIC_CALL_ADD_TRAMP_KEY(name)				\
	asm(".pushsection .static_call_tramp_key, \"a\"		\n"	\
	    ".long " STATIC_CALL_TRAMP_STR(name) " - .		\n"	\
	    ".long " STATIC_CALL_KEY_STR(name) " - .		\n"	\
	    ".popsection					\n")

extern int static_call_init(void);
extern int static_call_text_reserved(void *start, void *end);
extern void static_call_force_reinit(void);

#else /* !CONFIG_HAVE_STATIC_CALL_INLINE*/

#define __STATIC_CALL_ADD_TRAMP_KEY(name)
static inline int static_call_init(void) { return 0; }
static inline int static_call_text_reserved(void *start, void *end) { return 0; }
static inline void static_call_force_reinit(void) {}

#endif /* CONFIG_HAVE_STATIC_CALL_INLINE */

#endif /* _LINUX_STATIC_CALL_H */
