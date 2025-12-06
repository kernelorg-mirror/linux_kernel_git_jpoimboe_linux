/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ASM_H
#define _LINUX_ASM_H

#ifdef __ASSEMBLER__
# define __ASM_C(a,b) a
#else
# define __ASM_C(a,b) b
#endif

#define DEFINE_MACRO(name)						\
	__ASM_C(DEFINE_ ## name,					\
		asm(__stringify(DEFINE_ ## name)))

#endif /* _LINUX_ASM_H */
