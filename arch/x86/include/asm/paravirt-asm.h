#ifndef _ASM_X86_PARAVIRT_ASM_H
#define _ASM_X86_PARAVIRT_ASM_H

#ifdef CONFIG_PARAVIRT
#ifdef __ASSEMBLY__

#include <asm/asm.h>
#include <asm/paravirt_types.h>

#define PV_TYPE(ops, off) ((PARAVIRT_PATCH_##ops + (off)) / __ASM_SEL(4, 8))

#define PV_SITE(insns, ops, off, clobbers)				\
771:;									\
	insns;								\
772:;									\
	.pushsection .parainstructions, "a";				\
	 _ASM_ALIGN;							\
	 _ASM_PTR 771b;							\
	 .byte PV_TYPE(ops, off);					\
	 .byte 772b-771b;						\
	 .short clobbers;						\
	.popsection


#define COND_PUSH(set, mask, reg)			\
	.if ((~(set)) & mask); push %reg; .endif
#define COND_POP(set, mask, reg)			\
	.if ((~(set)) & mask); pop %reg; .endif

#ifdef CONFIG_X86_64

#define PV_SAVE_REGS(set)			\
	COND_PUSH(set, CLBR_RAX, rax);		\
	COND_PUSH(set, CLBR_RCX, rcx);		\
	COND_PUSH(set, CLBR_RDX, rdx);		\
	COND_PUSH(set, CLBR_RSI, rsi);		\
	COND_PUSH(set, CLBR_RDI, rdi);		\
	COND_PUSH(set, CLBR_R8,  r8);		\
	COND_PUSH(set, CLBR_R9,  r9);		\
	COND_PUSH(set, CLBR_R10, r10);		\
	COND_PUSH(set, CLBR_R11, r11)

#define PV_RESTORE_REGS(set)			\
	COND_POP(set, CLBR_R11, r11);		\
	COND_POP(set, CLBR_R10, r10);		\
	COND_POP(set, CLBR_R9,  r9);		\
	COND_POP(set, CLBR_R8,  r8);		\
	COND_POP(set, CLBR_RDI, rdi);		\
	COND_POP(set, CLBR_RSI, rsi);		\
	COND_POP(set, CLBR_RDX, rdx);		\
	COND_POP(set, CLBR_RCX, rcx);		\
	COND_POP(set, CLBR_RAX, rax)

#define PV_INDIRECT(addr)	*addr(%rip)

#else /* !CONFIG_X86_64 */

#define PV_SAVE_REGS(set)			\
	COND_PUSH(set, CLBR_EAX, eax);		\
	COND_PUSH(set, CLBR_EDI, edi);		\
	COND_PUSH(set, CLBR_ECX, ecx);		\
	COND_PUSH(set, CLBR_EDX, edx)

#define PV_RESTORE_REGS(set)			\
	COND_POP(set, CLBR_EDX, edx);		\
	COND_POP(set, CLBR_ECX, ecx);		\
	COND_POP(set, CLBR_EDI, edi);		\
	COND_POP(set, CLBR_EAX, eax)

#define PV_INDIRECT(addr)	*%cs:addr

#endif /* !CONFIG_X86_64 */

#define INTERRUPT_RETURN						\
	PV_SITE(jmp PV_INDIRECT(pv_cpu_ops+PV_CPU_iret),		\
		pv_cpu_ops, PV_CPU_iret, CLBR_NONE)

#define DISABLE_INTERRUPTS(clobbers)					\
	PV_SITE(PV_SAVE_REGS(clobbers | CLBR_CALLEE_SAVE);		\
		call PV_INDIRECT(pv_irq_ops+PV_IRQ_irq_disable);	\
		PV_RESTORE_REGS(clobbers | CLBR_CALLEE_SAVE),		\
		pv_irq_ops, PV_IRQ_irq_disable, clobbers)

#define ENABLE_INTERRUPTS(clobbers)					\
	PV_SITE(PV_SAVE_REGS(clobbers | CLBR_CALLEE_SAVE);		\
		call PV_INDIRECT(pv_irq_ops+PV_IRQ_irq_enable);		\
		PV_RESTORE_REGS(clobbers | CLBR_CALLEE_SAVE),		\
		pv_irq_ops, PV_IRQ_irq_enable, clobbers)

#ifdef CONFIG_X86_32

#define GET_CR0_INTO_EAX						\
	push %ecx; push %edx;						\
	call PV_INDIRECT(pv_cpu_ops+PV_CPU_read_cr0);			\
	pop %edx; pop %ecx

#else	/* !CONFIG_X86_32 */

/*
 * If swapgs is used while the userspace stack is still current,
 * there's no way to call a pvop.  The PV replacement *must* be
 * inlined, or the swapgs instruction must be trapped and emulated.
 */
#define SWAPGS_UNSAFE_STACK						\
	PV_SITE(swapgs, pv_cpu_ops, PV_CPU_swapgs, CLBR_NONE)

/*
 * Note: swapgs is very special, and in practise is either going to be
 * implemented with a single "swapgs" instruction or something very
 * special.  Either way, we don't need to save any registers for
 * it.
 */
#define SWAPGS								\
	PV_SITE(call PV_INDIRECT(pv_cpu_ops+PV_CPU_swapgs),		\
		pv_cpu_ops, PV_CPU_swapgs, CLBR_NONE)

#define GET_CR2_INTO_RAX						\
	call PV_INDIRECT(pv_mmu_ops+PV_MMU_read_cr2)

#define USERGS_SYSRET64							\
	PV_SITE(jmp PV_INDIRECT(pv_cpu_ops+PV_CPU_usergs_sysret64),	\
		pv_cpu_ops, PV_CPU_usergs_sysret64, CLBR_NONE)

#endif	/* !CONFIG_X86_32 */

#endif  /*  __ASSEMBLY__  */
#endif /* CONFIG_PARAVIRT */

#endif /* _ASM_X86_PARAVIRT_ASM_H */
