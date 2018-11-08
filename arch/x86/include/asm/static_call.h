/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_STATIC_CALL_H
#define _ASM_STATIC_CALL_H

#include <asm/asm-offsets.h>

/*
 * This trampoline is used for out-of-line static calls.  It has a direct jump
 * which gets patched by static_call_update().
 *
 * With CONFIG_HAVE_STATIC_CALL_INLINE enabled, if a call site fits within a
 * cache line, it gets promoted to an inline static call and the trampoline is
 * no longer used for that site.  In this case the name of this trampoline has
 * a magical aspect: objtool uses it to find static call sites so it can create
 * the .static_call_sites section.
 *
 * Trampolines are placed in the .static_call.text section to prevent two-byte
 * tail calls to the trampoline and two-byte jumps from the trampoline.
 *
 * IMPORTANT: The JMP instruction's 4-byte destination must never cross
 *            cacheline boundaries!  The patching code relies on that to ensure
 *            atomic updates.
 */
#define ARCH_DEFINE_STATIC_CALL_TRAMP(key, func)			\
	asm(".pushsection .static_call.text, \"ax\"		\n"	\
	    ".align 8						\n"	\
	    ".globl " STATIC_CALL_TRAMP_STR(key) "		\n"	\
	    ".type " STATIC_CALL_TRAMP_STR(key) ", @function	\n"	\
	    STATIC_CALL_TRAMP_STR(key) ":			\n"	\
	    "jmp " #func "					\n"	\
	    ".popsection					\n")

#endif /* _ASM_STATIC_CALL_H */
