/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_DEFERRED_H
#define _LINUX_UNWIND_USER_DEFERRED_H

#include <linux/task_work.h>
#include <linux/unwind_user.h>
#include <linux/unwind_deferred_types.h>

struct unwind_work;

typedef void (*unwind_callback_t)(struct unwind_work *work, struct unwind_stacktrace *trace, u64 cookie);

struct unwind_work {
	struct callback_head		work;
	unwind_callback_t		func;
	int				pending;
};

#ifdef CONFIG_UNWIND_USER

void unwind_task_init(struct task_struct *task);
void unwind_task_free(struct task_struct *task);

void unwind_deferred_init(struct unwind_work *work, unwind_callback_t func);
int unwind_deferred_request(struct unwind_work *work, u64 *cookie);
bool unwind_deferred_cancel(struct task_struct *task, struct unwind_work *work);

static __always_inline void unwind_enter_from_user_mode(void)
{
	current->unwind_info.cookie = 0;
}

static __always_inline void unwind_exit_to_user_mode(void)
{
	current->unwind_info.cookie = 0;
}

#else /* !CONFIG_UNWIND_USER */

static inline void unwind_task_init(struct task_struct *task) {}
static inline void unwind_task_free(struct task_struct *task) {}

static inline void unwind_deferred_init(struct unwind_work *work, unwind_callback_t func) {}
static inline int unwind_deferred_request(struct task_struct *task, struct unwind_work *work, u64 *cookie) { return -ENOSYS; }
static inline bool unwind_deferred_cancel(struct task_struct *task, struct unwind_work *work) { return false; }

static inline void unwind_enter_from_user_mode(void) {}
static inline void unwind_exit_to_user_mode(void) {}

#endif /* !CONFIG_UNWIND_USER */

#endif /* _LINUX_UNWIND_USER_DEFERRED_H */
