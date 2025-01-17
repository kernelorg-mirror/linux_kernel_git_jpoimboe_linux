// SPDX-License-Identifier: GPL-2.0
/*
 * Deferred user space unwinding
 */
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/sframe.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/mm.h>
#include <linux/unwind_deferred.h>

#define UNWIND_MAX_ENTRIES 512

/* entry-from-user counter */
static DEFINE_PER_CPU(u64, unwind_ctx_ctr);

/*
 * The context cookie is a unique identifier which allows post-processing to
 * correlate kernel trace(s) with user unwinds.  The high 12 bits are the CPU
 * id; the lower 48 bits are a per-CPU entry counter.
 */
static u64 ctx_to_cookie(u64 cpu, u64 ctx)
{
	BUILD_BUG_ON(NR_CPUS > 65535);
	return (ctx & ((1UL << 48) - 1)) | (cpu << 48);
}

/*
 * Read the task context cookie, first initializing it if this is the first
 * call to get_cookie() since the most recent entry from user.
 */
static u64 get_cookie(struct unwind_task_info *info)
{
	u64 ctx_ctr;
	u64 cookie;
	u64 cpu;

	guard(irqsave)();

	cookie = info->cookie;
	if (cookie)
		return cookie;


	cpu = raw_smp_processor_id();
	ctx_ctr = __this_cpu_inc_return(unwind_ctx_ctr);
	info->cookie = ctx_to_cookie(cpu, ctx_ctr);

	return cookie;

}

static void unwind_deferred_task_work(struct callback_head *head)
{
	struct unwind_work *work = container_of(head, struct unwind_work, work);
	struct unwind_task_info *info = &current->unwind_info;
	struct unwind_stacktrace trace;
	u64 cookie;

	if (WARN_ON_ONCE(!work->pending))
		return;

	/*
	 * From here on out, the callback must always be called, even if it's
	 * just an empty trace.
	 */

	cookie = get_cookie(info);

	/* Check for task exit path. */
	if (!current->mm)
		goto do_callback;

	if (!info->entries) {
		info->entries = kmalloc_array(UNWIND_MAX_ENTRIES, sizeof(long),
					      GFP_KERNEL);
		if (!info->entries)
			goto do_callback;
	}

	trace.entries = info->entries;
	trace.nr = 0;
	unwind_user(&trace, UNWIND_MAX_ENTRIES);

do_callback:
	work->func(work, &trace, cookie);
	work->pending = 0;
}

/*
 * Schedule a user space unwind to be done in task work before exiting the
 * kernel.
 *
 * The returned cookie output is a unique identifer for the current task entry
 * context.  Its value will also be passed to the callback function.  It can be
 * used to stitch kernel and user stack traces together in post-processing.
 *
 * It's valid to call this function multiple times for the same @work within
 * the same task entry context.  Each call will return the same cookie.  If the
 * callback is already pending, an error will be returned along with the
 * cookie.  If the callback is not pending because it has already been
 * previously called for the same entry context, it will be called again with
 * the same stack trace and cookie.
 *
 * Thus are three possible return scenarios:
 *
 *   * return != 0, *cookie == 0: the operation failed, no pending callback.
 *
 *   * return != 0, *cookie != 0: the callback is already pending. The cookie
 *     can still be used to correlate with the pending callback.
 *
 *   * return == 0, *cookie != 0: the callback queued successfully.  The
 *     callback is guaranteed to be called with the given cookie.
 */
int unwind_deferred_request(struct unwind_work *work, u64 *cookie)
{
	struct unwind_task_info *info = &current->unwind_info;
	int ret;

	*cookie = 0;

	if (WARN_ON_ONCE(in_nmi()))
		return -EINVAL;

	if (!current->mm || !user_mode(task_pt_regs(current)))
		return -EINVAL;

	guard(irqsave)();

	*cookie = get_cookie(info);

	/* callback already pending? */
	if (work->pending)
		return -EEXIST;

	ret = task_work_add(current, &work->work, TWA_RESUME);
	if (WARN_ON_ONCE(ret))
		return ret;

	work->pending = 1;

	return 0;
}

bool unwind_deferred_cancel(struct task_struct *task, struct unwind_work *work)
{
	bool ret;

	ret = task_work_cancel(task, &work->work);
	if (ret)
		work->pending = 0;

	return ret;
}

void unwind_deferred_init(struct unwind_work *work, unwind_callback_t func)
{
	memset(work, 0, sizeof(*work));

	init_task_work(&work->work, unwind_deferred_task_work);
	work->func = func;
}

void unwind_task_init(struct task_struct *task)
{
	struct unwind_task_info *info = &task->unwind_info;

	memset(info, 0, sizeof(*info));
}

void unwind_task_free(struct task_struct *task)
{
	struct unwind_task_info *info = &task->unwind_info;

	kfree(info->entries);
}
