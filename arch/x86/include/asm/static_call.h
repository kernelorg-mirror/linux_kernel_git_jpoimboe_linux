/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_STATIC_CALL_H
#define _ASM_STATIC_CALL_H

#ifdef CONFIG_HAVE_ARCH_STATIC_CALL

/* The static call table is created by objtool */
struct static_call_site {
	s32 addr;
	s32 key;
};

struct static_call_mod {
	struct list_head list;
	struct static_call_site *sites;
	struct module *mod; /* NULL means vmlinux */
};

struct static_call_key {
	/* The trampoline expects 'func' to be first. */
	void *func;

	struct list_head modules;
};

extern void static_call_init(void);
extern void __static_call_update(struct static_call_key *key, void *func);

#define STATIC_CALL_TRAMP(key) ____static_call_tramp_##key
#define STATIC_CALL_TRAMP_STR(key) __stringify(STATIC_CALL_TRAMP(key))

#define DECLARE_STATIC_CALL(key, func)					\
	extern struct static_call_key key;				\
	__ADDRESSABLE(key);						\
	extern typeof(func) STATIC_CALL_TRAMP(key)

#define DEFINE_STATIC_CALL(key, _func)					\
	DECLARE_STATIC_CALL(key, _func);				\
	struct static_call_key key = {					\
		.func = _func,						\
		.modules = LIST_HEAD_INIT(key.modules),			\
	};								\
	asm(".pushsection .text, \"ax\"				\n"	\
	    ".align 4						\n"	\
	    ".globl " STATIC_CALL_TRAMP_STR(key) "		\n"	\
	    ".type " STATIC_CALL_TRAMP_STR(key) ", @function	\n"	\
	    STATIC_CALL_TRAMP_STR(key) ":			\n"	\
	    ANNOTATE_RETPOLINE_SAFE "				\n"	\
	    "jmpq *" __stringify(key) "(%rip)			\n"	\
	    ".popsection					\n")

#define STATIC_CALL_EXPORT(key)						\
	EXPORT_SYMBOL(key);						\
	EXPORT_SYMBOL(STATIC_CALL_TRAMP(key))

#define STATIC_CALL_EXPORT_GPL(key)					\
	EXPORT_SYMBOL_GPL(key);						\
	EXPORT_SYMBOL_GPL(STATIC_CALL_TRAMP(key))

#define static_call(key, args...) STATIC_CALL_TRAMP(key)(args)

#define static_call_update(key, func)					\
({									\
	BUILD_BUG_ON(!__same_type(typeof(func), typeof(STATIC_CALL_TRAMP(key)))); \
	__static_call_update(&key, func);				\
})

#else /* !CONFIG_ARCH_HAVE_STATIC_CALL */
static inline void static_call_init(void) {}
#endif

#endif /* _ASM_STATIC_CALL_H */
