// SPDX-License-Identifier: GPL-2.0
/*
* Generic interface for unwinding user space from task context
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
#include <asm/unwind_user.h>

#define UNWIND_NUM_CALLBACKS 32
#define UNWIND_MAX_ENTRIES 512

static struct unwind_user_frame fp_frame = {
	ARCH_INIT_USER_FP_FRAME
};

static DEFINE_MUTEX(callbacks_lock);
static struct unwind_callback *callbacks[UNWIND_NUM_CALLBACKS];

static DEFINE_PER_CPU(u64, ctx_ctr);

int unwind_user_next(struct unwind_user_state *state)
{
	struct unwind_user_frame _frame;
	struct unwind_user_frame *frame = &_frame;
	unsigned long cfa, fp, ra;
	int ret = -EINVAL;

	if (state->done)
		return -EINVAL;

	switch (state->type) {
	case UNWIND_USER_TYPE_FP:
		frame = &fp_frame;
		break;
	case UNWIND_USER_TYPE_SFRAME:
		ret = sframe_find(state->ip, frame);
		if (ret)
			goto the_end;
		break;
	default:
		BUG();
	}

	cfa = (frame->use_fp ? state->fp : state->sp) + frame->cfa_off;

	if (frame->ra_off && get_user(ra, (unsigned long *)(cfa + frame->ra_off)))
		goto the_end;

	if (frame->fp_off && get_user(fp, (unsigned long *)(cfa + frame->fp_off)))
		goto the_end;

	state->sp = cfa;
	state->ip = ra;
	if (frame->fp_off)
		state->fp = fp;

	return 0;

the_end:
	state->done = true;
	return ret;
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
	else
		state->type = UNWIND_USER_TYPE_FP;

	state->sp = user_stack_pointer(regs);
	state->ip = instruction_pointer(regs);
	state->fp = frame_pointer(regs);

	return unwind_user_next(state);
}

int unwind_user(struct unwind_stacktrace *trace, unsigned int max_entries)
{
	struct unwind_user_state state;

	trace->nr = 0;

	if (!max_entries)
		return -EINVAL;

	for_each_user_frame(&state) {
		trace->entries[trace->nr++] = state.ip;
		if (trace->nr >= max_entries)
			break;
	}

	return 0;
}

static void do_deferred_work(struct callback_head *unused)
{
	struct unwind_stacktrace trace;
	unsigned long pending;
	unsigned long flags;
	u64 cookie;
	int i;

	if (WARN_ON_ONCE(!current->unwind_ctx_cookie ||
			 !current->unwind_pending))
		return;

	local_irq_save(flags);

	pending = current->unwind_pending;
	cookie = current->unwind_ctx_cookie;

	current->unwind_pending = 0;
	current->unwind_ctx_cookie = 0;

	local_irq_restore(flags);

	trace.nr = 0;
	trace.entries = current->unwind_entries;

	if (current->unwind_cached != cookie) {
		unwind_user(&trace, UNWIND_MAX_ENTRIES);
		current->unwind_cached = cookie;
	}

	guard(mutex)(&callbacks_lock);

	for_each_set_bit(i, &pending, UNWIND_NUM_CALLBACKS) {
		struct unwind_callback *callback;

		callback = callbacks[i];

		/*
		 * Make sure the callback is still registered.
		 *
		 * FIXME: There's a race where a pending callback gets
		 * unregistered and a new one gets registered in the same slot,
		 * causing a spurious callback.
		 */
		if (!callback)
			continue;

		callback->func(callback, &trace, cookie);
	}
}

int unwind_user_deferred(struct unwind_callback *callback, u64 *ctx_cookie)

{
	BUILD_BUG_ON(NR_CPUS > 65535);

	if (WARN_ON_ONCE(in_nmi()))
		return -EINVAL;

	local_irq_disable();

	if (!current->unwind_ctx_cookie) {
		u64 cpu = smp_processor_id();
		u64 cookie;

		cookie = __this_cpu_read(ctx_ctr);
		cookie &= ((1UL << 48) - 1);
		cookie |= ((cpu << 48) + 1);
		__this_cpu_write(ctx_ctr, cookie);

		current->unwind_ctx_cookie = cookie;
		task_work_add(current, &current->unwind_work, TWA_RESUME);
	}

	*ctx_cookie = current->unwind_ctx_cookie;
	current->unwind_pending |= (1 << callback->unwind_priv.idx);

	local_irq_enable();

	return 0;
}

int unwind_user_register(struct unwind_callback *callback)
{
	if (!current->unwind_entries) {
		unsigned long *entries;

		entries = kmalloc(UNWIND_MAX_ENTRIES * sizeof(long), GFP_KERNEL);
		if (!entries)
			return -ENOMEM;

		current->unwind_entries = entries;
	}

	guard(mutex)(&callbacks_lock);

	for (int i = 0; i < UNWIND_NUM_CALLBACKS; i++) {
		if (!callbacks[i]) {
			callback->unwind_priv.idx = i;
			callbacks[i] = callback;
			return 0;
		}
	}

	return -ENOSPC;
}

int unwind_user_unregister(struct unwind_callback *callback)
{
	guard(mutex)(&callbacks_lock);

	callbacks[callback->unwind_priv.idx] = NULL;

	return 0;
}

void unwind_task_init(struct task_struct *task)
{
	task->unwind_entries	= NULL;
	task->unwind_nr_entries	= 0;
	task->unwind_pending	= 0;
	task->unwind_ctx_cookie	= 0;
	task->unwind_cached	= 0;

	init_task_work(&task->unwind_work, do_deferred_work);
}

void unwind_task_free(struct task_struct *task)
{
	if (task->unwind_entries)
		kfree(task->unwind_entries);
}
