// SPDX-License-Identifier: GPL-2.0
/*
* Generic interfaces for unwinding user space
*
* Copyright (C) 2024 Josh Poimboeuf <jpoimboe@kernel.org>
*/
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/unwind_user.h>
#include <linux/sframe.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/mm.h>

#define UNWIND_MAX_ENTRIES 512

#ifdef CONFIG_HAVE_UNWIND_USER_FP
#include <asm/unwind_user.h>
static struct unwind_user_frame fp_frame = {
	ARCH_INIT_USER_FP_FRAME
};
#else
static struct unwind_user_frame fp_frame;
#endif

static struct unwind_callback *callbacks[UNWIND_MAX_CALLBACKS];
static DECLARE_RWSEM(callbacks_rwsem);

/* Counter for entries from user space */
DEFINE_PER_CPU(u64, unwind_ctx_ctr);

int unwind_user_next(struct unwind_user_state *state)
{
	struct unwind_user_frame _frame;
	struct unwind_user_frame *frame = &_frame;
	unsigned long prev_ip, cfa, fp, ra = 0;

	if (state->done)
		return -EINVAL;

	prev_ip = state->ip;

	switch (state->type) {
	case UNWIND_USER_TYPE_FP:
		frame = &fp_frame;
		break;
	case UNWIND_USER_TYPE_SFRAME:
		if (sframe_find(state->ip, frame)) {
			if (!IS_ENABLED(CONFIG_HAVE_UNWIND_USER_FP))
				goto the_end;

			frame = &fp_frame;
		}
		break;
	case UNWIND_USER_TYPE_NONE:
		goto the_end;
	default:
		BUG();
	}

	cfa = (frame->use_fp ? state->fp : state->sp) + frame->cfa_off;

	if (frame->ra_off && get_user(ra, (unsigned long __user *)(cfa + frame->ra_off)))
		goto the_end;

	if (ra == prev_ip)
		goto the_end;

	if (frame->fp_off && get_user(fp, (unsigned long __user *)(cfa + frame->fp_off)))
		goto the_end;

	state->sp = cfa;
	state->ip = ra;
	if (frame->fp_off)
		state->fp = fp;

	return 0;

the_end:
	state->done = true;
	return -EINVAL;
}

int unwind_user_start(struct unwind_user_state *state)
{
	struct pt_regs *regs = task_pt_regs(current);

	memset(state, 0, sizeof(*state));

	if (!current->mm) {
		state->done = true;
		return -EINVAL;
	}

	if (current_has_sframe())
		state->type = UNWIND_USER_TYPE_SFRAME;
	else if (IS_ENABLED(CONFIG_UNWIND_USER_FP))
		state->type = UNWIND_USER_TYPE_FP;
	else
		state->type = UNWIND_USER_TYPE_NONE;

	state->sp = user_stack_pointer(regs);
	state->ip = instruction_pointer(regs);
	state->fp = frame_pointer(regs);

	return 0;
}

int unwind_user(struct unwind_stacktrace *trace, unsigned int max_entries)
{
	struct unwind_user_state state;

	trace->nr = 0;

	if (!max_entries)
		return -EINVAL;

	if (!current->mm)
		return 0;

	for_each_user_frame(&state) {
		trace->entries[trace->nr++] = state.ip;
		if (trace->nr >= max_entries)
			break;
	}

	return 0;
}

/*
 * The "context cookie" is a unique identifier which allows post-processing to
 * correlate kernel trace(s) with user unwinds.  It has the CPU id the highest
 * 16 bits and a per-CPU entry counter in the lower 48 bits.
 */
static u64 ctx_to_cookie(u64 cpu, u64 ctx)
{
	BUILD_BUG_ON(NR_CPUS > 65535);
	return (ctx & ((1UL << 48) - 1)) | cpu;
}

/*
 * Schedule a user space unwind to be done in task work before exiting the
 * kernel.
 *
 * The @callback must have previously been registered with
 * unwind_user_register().
 *
 * The @cookie output is a unique identifer which will also be passed to the
 * callback function.  It can be used to stitch kernel and user traces together
 * in post-processing.
 *
 * If there are multiple calls to this function for a given @callback, the
 * cookie will usually be the same and the callback will only be called once.
 *
 * The only exception is when the task has migrated to another CPU, *and* this
 * is called while the task work is running (or has already run).  Then a new
 * cookie will be generated and the callback will be called again for the new
 * cookie.
 */
int unwind_user_deferred(struct unwind_callback *callback, u64 *ctx_cookie, void *data)
{
	struct unwind_task_info *info = &current->unwind_task_info;
	u64 cookie = info->ctx_cookie;
	int idx = callback->idx;

	if (WARN_ON_ONCE(in_nmi()))
		return -EINVAL;

	if (WARN_ON_ONCE(!callback->func || idx < 0))
		return -EINVAL;

	if (!current->mm)
		return -EINVAL;

	guard(irqsave)();

	if (cookie && (info->pending_callbacks & (1 << idx)))
		goto done;

	/*
	 * If this is the first call from *any* caller since the most recent
	 * entry from user space, initialize the task context cookie and
	 * schedule the task work.
	 */
	if (!cookie) {
		u64 ctx_ctr = __this_cpu_read(unwind_ctx_ctr);
		u64 cpu = raw_smp_processor_id();

		cookie = ctx_to_cookie(cpu, ctx_ctr);

		/*
		 * If called after task work has sent an unwind to the callback
		 * function but before the exit to user space, skip it as the
		 * previous call to the callback function should suffice.
		 *
		 * The only exception is if this task has migrated to another
		 * CPU since the first call to unwind_user_deferred().  The
		 * per-CPU context counter will have changed which will result
		 * in a new cookie and another unwind (see comment above
		 * function).
		 */
		if (cookie == info->last_cookies[idx])
			goto done;

		info->ctx_cookie = cookie;
		task_work_add(current, &info->work, TWA_RESUME);
	}

	info->pending_callbacks |= (1 << idx);
	info->privs[idx] = data;
	info->last_cookies[idx] = cookie;

done:
	if (ctx_cookie)
		*ctx_cookie = cookie;
	return 0;
}

static void unwind_user_task_work(struct callback_head *head)
{
	struct unwind_task_info *info = container_of(head, struct unwind_task_info, work);
	struct task_struct *task = container_of(info, struct task_struct, unwind_task_info);
	void *privs[UNWIND_MAX_CALLBACKS];
	struct unwind_stacktrace trace;
	unsigned long pending;
	u64 cookie = 0;
	int i;

	BUILD_BUG_ON(UNWIND_MAX_CALLBACKS > 32);

	if (WARN_ON_ONCE(task != current))
		return;

	if (WARN_ON_ONCE(!info->ctx_cookie || !info->pending_callbacks))
		return;

	scoped_guard(irqsave) {
		pending = info->pending_callbacks;
		cookie = info->ctx_cookie;

		info->pending_callbacks = 0;
		info->ctx_cookie = 0;
		memcpy(privs, info->privs, sizeof(void *) * UNWIND_MAX_CALLBACKS);
	}

	if (!info->entries) {
		info->entries = kmalloc(UNWIND_MAX_ENTRIES * sizeof(long),
					GFP_KERNEL);
		if (!info->entries)
			return;
	}

	trace.entries = info->entries;
	trace.nr = 0;
	unwind_user(&trace, UNWIND_MAX_ENTRIES);

	guard(rwsem_read)(&callbacks_rwsem);

	for_each_set_bit(i, &pending, UNWIND_MAX_CALLBACKS) {
		if (callbacks[i])
			callbacks[i]->func(&trace, cookie, privs[i]);
	}
}

int unwind_user_register(struct unwind_callback *callback, unwind_callback_t func)
{
	scoped_guard(rwsem_write, &callbacks_rwsem) {
		for (int i = 0; i < UNWIND_MAX_CALLBACKS; i++) {
			if (!callbacks[i]) {
				callback->func = func;
				callback->idx = i;
				callbacks[i] = callback;
				return 0;
			}
		}
	}

	callback->func = NULL;
	callback->idx = -1;
	return -ENOSPC;
}

int unwind_user_unregister(struct unwind_callback *callback)
{
	if (callback->idx < 0)
		return -EINVAL;

	scoped_guard(rwsem_write, &callbacks_rwsem)
		callbacks[callback->idx] = NULL;

	callback->func = NULL;
	callback->idx = -1;

	return 0;
}

void unwind_task_init(struct task_struct *task)
{
	struct unwind_task_info *info = &task->unwind_task_info;

	info->entries		= NULL;
	info->pending_callbacks	= 0;
	info->ctx_cookie	= 0;

	memset(info->last_cookies, 0, sizeof(u64) * UNWIND_MAX_CALLBACKS);
	memset(info->privs,	   0, sizeof(u64) * UNWIND_MAX_CALLBACKS);

	init_task_work(&info->work, unwind_user_task_work);
}

void unwind_task_free(struct task_struct *task)
{
	struct unwind_task_info *info = &task->unwind_task_info;

	kfree(info->entries);
}
