/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_STATIC_CALL_H
#define _ASM_ARM64_STATIC_CALL_H

/*
 * Make a dummy reference to a function pointer in C to force the compiler to
 * emit a __kcfi_typeid_ symbol for asm to use.
 */
#define GEN_CFI_SYM(func)						\
	static typeof(func) __used __section(".discard.cfi") *__UNIQUE_ID(cfi) = func


/* Generate a CFI-compliant static call NOP function */
#define __ARCH_DEFINE_STATIC_CALL_CFI(name, insns)			\
	asm(".align 4						\n"	\
	    ".word __kcfi_typeid_" name "			\n"	\
	    ".globl " name "					\n"	\
	    name ":						\n"	\
	    "bti c						\n"	\
	    insns "						\n"	\
	    "ret						\n"	\
	    ".type " name ", @function				\n"	\
	    ".size " name ", . - " name "			\n")

#define __ARCH_DEFINE_STATIC_CALL_NOP_CFI(name)			\
	GEN_CFI_SYM(STATIC_CALL_NOP_CFI(name));			\
	__ARCH_DEFINE_STATIC_CALL_CFI(STATIC_CALL_NOP_CFI_STR(name), "")

#define __ARCH_DEFINE_STATIC_CALL_RET0_CFI(name)			\
	GEN_CFI_SYM(STATIC_CALL_RET0_CFI(name));			\
	__ARCH_DEFINE_STATIC_CALL_CFI(STATIC_CALL_RET0_CFI_STR(name), "mov x0, xzr")

#endif /* _ASM_ARM64_STATIC_CALL_H */
