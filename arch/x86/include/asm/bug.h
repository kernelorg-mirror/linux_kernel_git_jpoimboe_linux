/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_BUG_H
#define _ASM_X86_BUG_H

#include <linux/stringify.h>
#include <linux/instrumentation.h>
#include <linux/objtool.h>

/*
 * Despite that some emulators terminate on UD2, we use it for WARN().
 */
#define ASM_UD2		".byte 0x0f, 0x0b"
#define INSN_UD2	0x0b0f
#define LEN_UD2		2

/*
 * In clang we have UD1s reporting UBSAN failures on X86, 64 and 32bit.
 */
#define INSN_ASOP		0x67
#define OPCODE_ESCAPE		0x0f
#define SECOND_BYTE_OPCODE_UD1	0xb9
#define SECOND_BYTE_OPCODE_UD2	0x0b

#define BUG_NONE		0xffff
#define BUG_UD1			0xfffe
#define BUG_UD2			0xfffd

#ifdef CONFIG_GENERIC_BUG

#ifdef CONFIG_X86_32
#define ASM_BUG_REL(val)	.long val
#else
#define ASM_BUG_REL(val)	.long val - .
#endif

#ifdef CONFIG_DEBUG_BUGVERBOSE
#define ASM_BUGTABLE_VERBOSE(file, line)				\
	ASM_BUG_REL(file) ;						\
	.word line
#define ASM_BUGTABLE_VERBOSE_SIZE	6
#else
#define ASM_BUGTABLE_VERBOSE(file, line)
#define ASM_BUGTABLE_VERBOSE_SIZE	0
#endif

#define ASM_BUGTABLE_FLAGS(at, file, line, flags)			\
	.pushsection __bug_table, "aw" ;				\
	123:	ASM_BUG_REL(at) ;					\
	ASM_BUGTABLE_VERBOSE(file, line) ;				\
	.word	flags ;							\
	.org 123b + 6 + ASM_BUGTABLE_VERBOSE_SIZE ;			\
	.popsection

#define _BUG_FLAGS(ins, flags, extra)					\
do {									\
	asm_inline volatile("1:\t" ins "\n"				\
	    __stringify(ASM_BUGTABLE_FLAGS(1b, %c0, %c1, %c2)) "\n"	\
			    extra					\
		     : : "i" (__FILE__), "i" (__LINE__),		\
			 "i" (flags));					\
} while (0)

#else

#define _BUG_FLAGS(ins, flags, extra)  asm volatile(ins)

#endif /* CONFIG_GENERIC_BUG */

#define HAVE_ARCH_BUG
#define BUG()							\
do {								\
	instrumentation_begin();				\
	_BUG_FLAGS(ASM_UD2, 0, "");				\
	__builtin_unreachable();				\
} while (0)

/*
 * This instrumentation_begin() is strictly speaking incorrect; but it
 * suppresses the complaints from WARN()s in noinstr code. If such a WARN()
 * were to trigger, we'd rather wreck the machine in an attempt to get the
 * message out than not know about it.
 */
#define __WARN_FLAGS(flags)					\
do {								\
	__auto_type __flags = BUGFLAG_WARNING|(flags);		\
	instrumentation_begin();				\
	_BUG_FLAGS(ASM_UD2, __flags, ASM_REACHABLE);		\
	instrumentation_end();					\
} while (0)


#ifdef __ASSEMBLY__
#ifdef CONFIG_BUG

#ifdef CONFIG_DEBUG_BUGVERBOSE
#define FILE_STR						\
.pushsection .rodata.str1.1, "aMS",@progbits,1;			\
	1: .string __FILE__;					\
.popsection
#else
#define FILE_STR
#endif

#define WARN_ONCE								\
	FILE_STR;								\
	2: ud2;									\
	ASM_BUGTABLE_FLAGS(2b, 1b, __LINE__, BUGFLAG_WARNING | BUGFLAG_ONCE);	\
	REACHABLE

#else /* !CONFIG_BUG */
#define WARN_ONCE
#endif /* CONFIG_BUG */
#endif /* __ASSEMBLY__ */


#include <asm-generic/bug.h>

#endif /* _ASM_X86_BUG_H */
