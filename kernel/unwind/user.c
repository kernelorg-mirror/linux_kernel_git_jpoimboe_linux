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
#include <linux/uaccess.h>
#include <asm/unwind_user.h>

static struct unwind_user_frame fp_frame = {
	ARCH_INIT_USER_FP_FRAME
};

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

	state->type = UNWIND_USER_TYPE_FP;

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
