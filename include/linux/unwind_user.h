/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_H
#define _LINUX_UNWIND_USER_H

#include <linux/types.h>
#include <linux/percpu-defs.h>

#define UNWIND_MAX_CALLBACKS 4

enum unwind_user_type {
	UNWIND_USER_TYPE_NONE,
	UNWIND_USER_TYPE_FP,
	UNWIND_USER_TYPE_SFRAME,
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
	unsigned long ip;
	unsigned long sp;
	unsigned long fp;
	enum unwind_user_type type;
	bool done;
};

struct unwind_task_info {
	u64			ctx_cookie;
	u32			pending_callbacks;
	u64			last_cookies[UNWIND_MAX_CALLBACKS];
	void			*privs[UNWIND_MAX_CALLBACKS];
	unsigned long		*entries;
	struct callback_head	work;
};

typedef void (*unwind_callback_t)(struct unwind_stacktrace *trace,
				  u64 ctx_cookie, void *data);

struct unwind_callback {
	unwind_callback_t		func;
	int				idx;
};


#ifdef CONFIG_UNWIND_USER

/* Synchronous interfaces: */

int unwind_user_start(struct unwind_user_state *state);
int unwind_user_next(struct unwind_user_state *state);

int unwind_user(struct unwind_stacktrace *trace, unsigned int max_entries);

#define for_each_user_frame(state) \
	for (unwind_user_start((state)); !(state)->done; unwind_user_next((state)))


/* Asynchronous interfaces: */

void unwind_task_init(struct task_struct *task);
void unwind_task_free(struct task_struct *task);

int unwind_user_register(struct unwind_callback *callback, unwind_callback_t func);
int unwind_user_unregister(struct unwind_callback *callback);

int unwind_user_deferred(struct unwind_callback *callback, u64 *ctx_cookie, void *data);

DECLARE_PER_CPU(u64, unwind_ctx_ctr);

static __always_inline void unwind_enter_from_user_mode(void)
{
	__this_cpu_inc(unwind_ctx_ctr);
}


#else /* !CONFIG_UNWIND_USER */

static inline void unwind_task_init(struct task_struct *task) {}
static inline void unwind_task_free(struct task_struct *task) {}

static inline int unwind_user_register(struct unwind_callback *callback, unwind_callback_t func) { return -ENOSYS; }
static inline int unwind_user_unregister(struct unwind_callback *callback) { return -ENOSYS; }

static inline int unwind_user_deferred(struct unwind_callback *callback, u64 *ctx_cookie, void *data) { return -ENOSYS; }

static inline void unwind_enter_from_user_mode(void) {}

#endif /* !CONFIG_UNWIND_USER */

#endif /* _LINUX_UNWIND_USER_H */
