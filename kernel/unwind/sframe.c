// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt)	"sframe: " fmt

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/sframe.h>
#include <linux/unwind_user.h>

#include "sframe.h"

#define SFRAME_FILENAME_LEN 32

struct sframe_section {
	struct rcu_head rcu;

	unsigned long sframe_addr;
	unsigned long text_addr;

	unsigned long fdes_addr;
	unsigned long fres_addr;
	unsigned int  fdes_nr;
	signed char   ra_off;
	signed char   fp_off;
};

DEFINE_STATIC_SRCU(sframe_srcu);

#define __SFRAME_GET_USER(out, user_ptr, type)				\
({									\
	type __tmp;							\
	if (get_user(__tmp, (type __user *)user_ptr))			\
		return -EFAULT;						\
	user_ptr += sizeof(__tmp);					\
	out = __tmp;							\
})

#define SFRAME_GET_USER(out, user_ptr, size)				\
({									\
	switch (size) {							\
	case 1:								\
		__SFRAME_GET_USER(out, user_ptr, u8);			\
		break;							\
	case 2:								\
		__SFRAME_GET_USER(out, user_ptr, u16);			\
		break;							\
	case 4:								\
		__SFRAME_GET_USER(out, user_ptr, u32);			\
		break;							\
	default:							\
		return -EINVAL;						\
	}								\
})

static unsigned char fre_type_to_size(unsigned char fre_type)
{
	if (fre_type > 2)
		return 0;
	return 1 << fre_type;
}

static unsigned char offset_size_enum_to_size(unsigned char off_size)
{
	if (off_size > 2)
		return 0;
	return 1 << off_size;
}

static int find_fde(struct sframe_section *sec, unsigned long ip,
		    struct sframe_fde *fde)
{
	struct sframe_fde __user *first, *last, *found = NULL;
	u32 ip_off, func_off_low = 0, func_off_high = -1;

	ip_off = ip - sec->sframe_addr;

	first = (void __user *)sec->fdes_addr;
	last = first + sec->fdes_nr;
	while (first <= last) {
		struct sframe_fde __user *mid;
		u32 func_off;

		mid = first + ((last - first) / 2);

		if (get_user(func_off, (s32 __user *)mid))
			return -EFAULT;

		if (ip_off >= func_off) {
			/* validate sort order */
			if (func_off < func_off_low)
				return -EINVAL;

			func_off_low = func_off;

			found = mid;
			first = mid + 1;
		} else {
			/* validate sort order */
			if (func_off > func_off_high)
				return -EINVAL;

			func_off_high = func_off;

			last = mid - 1;
		}
	}

	if (!found)
		return -EINVAL;

	if (copy_from_user(fde, found, sizeof(*fde)))
		return -EFAULT;

	/* check for gaps */
	if (ip_off < fde->start_addr || ip_off >= fde->start_addr + fde->size)
		return -EINVAL;

	return 0;
}

static int find_fre(struct sframe_section *sec, struct sframe_fde *fde,
		    unsigned long ip, struct unwind_user_frame *frame)
{
	unsigned char fde_type = SFRAME_FUNC_FDE_TYPE(fde->info);
	unsigned char fre_type = SFRAME_FUNC_FRE_TYPE(fde->info);
	unsigned char offset_count, offset_size;
	s32 cfa_off, ra_off, fp_off, ip_off;
	void __user *f, *last_f = NULL;
	unsigned char addr_size;
	u32 last_fre_ip_off = 0;
	u8 fre_info = 0;
	int i;

	addr_size = fre_type_to_size(fre_type);
	if (!addr_size)
		return -EINVAL;

	ip_off = ip - (sec->sframe_addr + fde->start_addr);

	f = (void __user *)sec->fres_addr + fde->fres_off;

	for (i = 0; i < fde->fres_num; i++) {
		u32 fre_ip_off;

		SFRAME_GET_USER(fre_ip_off, f, addr_size);

		if (fre_ip_off < last_fre_ip_off)
			return -EINVAL;

		last_fre_ip_off = fre_ip_off;

		if (fde_type == SFRAME_FDE_TYPE_PCINC) {
			if (ip_off < fre_ip_off)
				break;
		} else {
			/* SFRAME_FDE_TYPE_PCMASK */
			if (ip_off % fde->rep_size < fre_ip_off)
				break;
		}

		SFRAME_GET_USER(fre_info, f, 1);

		offset_count = SFRAME_FRE_OFFSET_COUNT(fre_info);
		offset_size  = offset_size_enum_to_size(SFRAME_FRE_OFFSET_SIZE(fre_info));

		if (!offset_count || !offset_size)
			return -EINVAL;

		last_f = f;
		f += offset_count * offset_size;
	}

	if (!last_f)
		return -EINVAL;

	f = last_f;

	SFRAME_GET_USER(cfa_off, f, offset_size);
	offset_count--;

	ra_off = sec->ra_off;
	if (!ra_off) {
		if (!offset_count--)
			return -EINVAL;

		SFRAME_GET_USER(ra_off, f, offset_size);
	}

	fp_off = sec->fp_off;
	if (!fp_off && offset_count) {
		offset_count--;
		SFRAME_GET_USER(fp_off, f, offset_size);
	}

	if (offset_count)
		return -EINVAL;

	frame->cfa_off = cfa_off;
	frame->ra_off = ra_off;
	frame->fp_off = fp_off;
	frame->use_fp = SFRAME_FRE_CFA_BASE_REG_ID(fre_info) == SFRAME_BASE_REG_FP;

	return 0;
}

int sframe_find(unsigned long ip, struct unwind_user_frame *frame)
{
	struct mm_struct *mm = current->mm;
	struct sframe_section *sec;
	struct sframe_fde fde;
	int ret = -EINVAL;

	if (!mm)
		return -EINVAL;

	guard(srcu)(&sframe_srcu);

	sec = mtree_load(&mm->sframe_mt, ip);
	if (!sec)
		return ret;

	ret = find_fde(sec, ip, &fde);
	if (ret)
		return ret;

	ret = find_fre(sec, &fde, ip, frame);
	if (ret)
		return ret;

	return 0;
}

static int __sframe_add_section(unsigned long sframe_addr,
				unsigned long text_start,
				unsigned long text_end)
{
	struct maple_tree *sframe_mt = &current->mm->sframe_mt;
	struct sframe_section *sec;
	struct sframe_header shdr;
	unsigned long header_end;
	int ret;

	if (copy_from_user(&shdr, (void __user *)sframe_addr, sizeof(shdr)))
		return -EFAULT;

	if (shdr.preamble.magic != SFRAME_MAGIC ||
	    shdr.preamble.version != SFRAME_VERSION_2 ||
	    !(shdr.preamble.flags & SFRAME_F_FDE_SORTED) ||
	    shdr.auxhdr_len || !shdr.num_fdes || !shdr.num_fres ||
	    shdr.fdes_off > shdr.fres_off) {
		return -EINVAL;
	}

	sec = kmalloc(sizeof(*sec), GFP_KERNEL);
	if (!sec)
		return -ENOMEM;

	header_end = sframe_addr + SFRAME_HDR_SIZE(shdr);

	sec->sframe_addr	= sframe_addr;
	sec->text_addr		= text_start;
	sec->fdes_addr		= header_end + shdr.fdes_off;
	sec->fres_addr		= header_end + shdr.fres_off;
	sec->fdes_nr		= shdr.num_fdes;
	sec->ra_off		= shdr.cfa_fixed_ra_offset;
	sec->fp_off		= shdr.cfa_fixed_fp_offset;

	ret = mtree_insert_range(sframe_mt, text_start, text_end, sec, GFP_KERNEL);
	if (ret) {
		kfree(sec);
		return ret;
	}

	return 0;
}

int sframe_add_section(unsigned long sframe_addr, unsigned long text_start,
		       unsigned long text_end)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *sframe_vma;

	mmap_read_lock(mm);

	sframe_vma = vma_lookup(mm, sframe_addr);
	if (!sframe_vma)
		goto err_unlock;

	if (text_start && text_end) {
		struct vm_area_struct *text_vma;

		text_vma = vma_lookup(mm, text_start);
		if (!(text_vma->vm_flags & VM_EXEC))
			goto err_unlock;

		if (PAGE_ALIGN(text_end) != text_vma->vm_end)
			goto err_unlock;
	} else {
		struct vm_area_struct *vma, *text_vma = NULL;
		VMA_ITERATOR(vmi, mm, 0);

		for_each_vma(vmi, vma) {
			if (vma->vm_file != sframe_vma->vm_file ||
			    !(vma->vm_flags & VM_EXEC))
				continue;

			if (text_vma) {
				pr_warn_once("%s[%d]: multiple EXEC segments unsupported\n",
					     current->comm, current->pid);
				goto err_unlock;
			}

			text_vma = vma;
		}

		if (!text_vma)
			goto err_unlock;

		text_start = text_vma->vm_start;
		text_end   = text_vma->vm_end;
	}

	mmap_read_unlock(mm);

	return __sframe_add_section(sframe_addr, text_start, text_end);

err_unlock:
	mmap_read_unlock(mm);
	return -EINVAL;
}

static void sframe_free_srcu(struct rcu_head *rcu)
{
	struct sframe_section *sec = container_of(rcu, struct sframe_section, rcu);

	kfree(sec);
}

static int __sframe_remove_section(struct mm_struct *mm,
				   struct sframe_section *sec)
{
	sec = mtree_erase(&mm->sframe_mt, sec->text_addr);
	if (!sec)
		return -EINVAL;

	call_srcu(&sframe_srcu, &sec->rcu, sframe_free_srcu);

	return 0;
}

int sframe_remove_section(unsigned long sframe_addr)
{
	struct mm_struct *mm = current->mm;
	struct sframe_section *sec;
	unsigned long index = 0;

	mt_for_each(&mm->sframe_mt, sec, index, ULONG_MAX) {
		if (sec->sframe_addr == sframe_addr)
			return __sframe_remove_section(mm, sec);
	}

	return -EINVAL;
}

void sframe_free_mm(struct mm_struct *mm)
{
	struct sframe_section *sec;
	unsigned long index = 0;

	if (!mm)
		return;

	mt_for_each(&mm->sframe_mt, sec, index, ULONG_MAX)
		kfree(sec);

	mtree_destroy(&mm->sframe_mt);
}
