/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_H
#define _LINUX_UNWIND_USER_H

#include <linux/types.h>

enum unwind_user_type {
	UNWIND_USER_TYPE_FP,
};

struct unwind_stacktrace {
	unsigned int	nr;
	unsigned long	*entries;
};

struct unwind_user_frame {
	s32 cfa_off;
	s32 ra_off;
	s32 fp_off;
	bool use_fp;
};

struct unwind_user_state {
	unsigned long ip, sp, fp;
	enum unwind_user_type type;
	bool done;
};

void unwind_task_init(struct task_struct *task);
void unwind_task_free(struct task_struct *task);

/* Synchronous interface: */

int unwind_user_start(struct unwind_user_state *state);
int unwind_user_next(struct unwind_user_state *state);

int unwind_user(struct unwind_stacktrace *trace, unsigned int max_entries);

#define for_each_user_frame(state) \
	for (unwind_user_start(state); !state.done; unwind_user_next(state))

#endif /* _LINUX_UNWIND_USER_H */
