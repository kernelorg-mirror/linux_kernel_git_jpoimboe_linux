#include <asm/paravirt.h>
#include <asm/asm-offsets.h>
#include <asm/special_insns.h>
#include <linux/stringify.h>

DEF_NATIVE(pv_irq_ops,	save_fl,		NATIVE_SAVE_FL);
DEF_NATIVE(pv_irq_ops,	restore_fl,		NATIVE_RESTORE_FL);
DEF_NATIVE(pv_irq_ops,	irq_disable,		NATIVE_IRQ_DISABLE);
DEF_NATIVE(pv_irq_ops,	irq_enable,		NATIVE_IRQ_ENABLE);
DEF_NATIVE(pv_mmu_ops,	read_cr2,		NATIVE_READ_CR2);
DEF_NATIVE(pv_mmu_ops,	read_cr3,		NATIVE_READ_CR3);
DEF_NATIVE(pv_mmu_ops,	write_cr3,		NATIVE_WRITE_CR3);
DEF_NATIVE(pv_mmu_ops,	flush_tlb_single,	NATIVE_FLUSH_TLB_SINGLE);

DEF_NATIVE(pv_cpu_ops,	usergs_sysret64,	NATIVE_USERGS_SYSRET64);
DEF_NATIVE(pv_cpu_ops,	swapgs,			NATIVE_SWAPGS);

DEF_NATIVE(,		mov32,			NATIVE_IDENTITY_32);
DEF_NATIVE(,		mov64,			NATIVE_IDENTITY);

#if defined(CONFIG_PARAVIRT_SPINLOCKS)
DEF_NATIVE(pv_lock_ops,	queued_spin_unlock,	NATIVE_QUEUED_SPIN_UNLOCK);
DEF_NATIVE(pv_lock_ops,	vcpu_is_preempted,	NATIVE_ZERO);
#endif

unsigned paravirt_patch_ident_32(void *insnbuf, unsigned len)
{
	return paravirt_patch_insns(insnbuf, len,
				    start__mov32, end__mov32);
}

unsigned paravirt_patch_ident_64(void *insnbuf, unsigned len)
{
	return paravirt_patch_insns(insnbuf, len,
				    start__mov64, end__mov64);
}

extern bool pv_is_native_spin_unlock(void);
extern bool pv_is_native_vcpu_is_preempted(void);

unsigned native_patch(u8 type, u16 clobbers, void *ibuf,
		      unsigned long addr, unsigned len)
{
	const unsigned char *start, *end;
	unsigned ret;

#define PATCH_SITE(ops, x)					\
		case PARAVIRT_PATCH(ops.x):			\
			start = start_##ops##_##x;		\
			end = end_##ops##_##x;			\
			goto patch_site
	switch(type) {
		PATCH_SITE(pv_irq_ops, restore_fl);
		PATCH_SITE(pv_irq_ops, save_fl);
		PATCH_SITE(pv_irq_ops, irq_enable);
		PATCH_SITE(pv_irq_ops, irq_disable);
		PATCH_SITE(pv_cpu_ops, usergs_sysret64);
		PATCH_SITE(pv_cpu_ops, swapgs);
		PATCH_SITE(pv_mmu_ops, read_cr2);
		PATCH_SITE(pv_mmu_ops, read_cr3);
		PATCH_SITE(pv_mmu_ops, write_cr3);
		PATCH_SITE(pv_mmu_ops, flush_tlb_single);
#if defined(CONFIG_PARAVIRT_SPINLOCKS)
		case PARAVIRT_PATCH(pv_lock_ops.queued_spin_unlock):
			if (pv_is_native_spin_unlock()) {
				start = start_pv_lock_ops_queued_spin_unlock;
				end   = end_pv_lock_ops_queued_spin_unlock;
				goto patch_site;
			}
			goto patch_default;

		case PARAVIRT_PATCH(pv_lock_ops.vcpu_is_preempted):
			if (pv_is_native_vcpu_is_preempted()) {
				start = start_pv_lock_ops_vcpu_is_preempted;
				end   = end_pv_lock_ops_vcpu_is_preempted;
				goto patch_site;
			}
			goto patch_default;
#endif

	default:
patch_default: __maybe_unused
		ret = paravirt_patch_default(type, clobbers, ibuf, addr, len);
		break;

patch_site:
		ret = paravirt_patch_insns(ibuf, len, start, end);
		break;
	}
#undef PATCH_SITE
	return ret;
}
