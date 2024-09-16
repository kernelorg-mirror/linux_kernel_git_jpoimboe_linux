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
#include <linux/uaccess.h>
#include <asm/unwind_user.h>

static struct unwind_user_frame fp_frame = {
	ARCH_INIT_USER_FP_FRAME
};

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
