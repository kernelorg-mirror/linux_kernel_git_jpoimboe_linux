// SPDX-License-Identifier: GPL-2.0
/* rwsem.c: R/W semaphores: contention handling functions
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from arch/i386/kernel/semaphore.c
 *
 * Writer lock-stealing by Alex Shi <alex.shi@intel.com>
 * and Michel Lespinasse <walken@google.com>
 *
 * Optimistic spinning by Tim Chen <tim.c.chen@intel.com>
 * and Davidlohr Bueso <davidlohr@hp.com>. Based on mutexes.
 *
 * Rwsem count bit fields re-definition by Waiman Long <longman@redhat.com>.
 */
#include <linux/rwsem.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/sched/signal.h>
#include <linux/sched/rt.h>
#include <linux/sched/wake_q.h>
#include <linux/sched/debug.h>
#include <linux/sched/clock.h>
#include <linux/osq_lock.h>

#include "rwsem.h"

/*
 * Guide to the rw_semaphore's count field.
 *
 * When the RWSEM_WRITER_LOCKED bit in count is set, the lock is owned
 * by a writer.
 *
 * The lock is owned by readers when
 * (1) the RWSEM_WRITER_LOCKED isn't set in count,
 * (2) some of the reader bits are set in count, and
 * (3) the owner field has RWSEM_READ_OWNED bit set.
 *
 * Having some reader bits set is not enough to guarantee a readers owned
 * lock as the readers may be in the process of backing out from the count
 * and a writer has just released the lock. So another writer may steal
 * the lock immediately after that.
 */

/*
 * Initialize an rwsem:
 */
void __init_rwsem(struct rw_semaphore *sem, const char *name,
		  struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	/*
	 * Make sure we are not reinitializing a held semaphore:
	 */
	debug_check_no_locks_freed((void *)sem, sizeof(*sem));
	lockdep_init_map(&sem->dep_map, name, key, 0);
#endif
	atomic_long_set(&sem->count, RWSEM_UNLOCKED_VALUE);
	raw_spin_lock_init(&sem->wait_lock);
	INIT_LIST_HEAD(&sem->wait_list);
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
	sem->owner = NULL;
	osq_lock_init(&sem->osq);
#endif
}

EXPORT_SYMBOL(__init_rwsem);

enum rwsem_waiter_type {
	RWSEM_WAITING_FOR_WRITE,
	RWSEM_WAITING_FOR_READ
};

struct rwsem_waiter {
	struct list_head list;
	struct task_struct *task;
	enum rwsem_waiter_type type;
	unsigned long timeout;
};

enum rwsem_wake_type {
	RWSEM_WAKE_ANY,		/* Wake whatever's at head of wait list */
	RWSEM_WAKE_READERS,	/* Wake readers only */
	RWSEM_WAKE_READ_OWNED	/* Waker thread holds the read lock */
};

/*
 * The typical HZ value is either 250 or 1000. So set the minimum waiting
 * time to 4ms in the wait queue before initiating the handoff protocol.
 */
#define RWSEM_WAIT_TIMEOUT	(HZ/250)

/*
 * We limit the maximum number of readers that can be woken up for a
 * wake-up call to not penalizing the waking thread for spending too
 * much time doing it as well as the unlikely possiblity of overflowing
 * the reader count.
 */
#define MAX_READERS_WAKEUP	0x100

/*
 * handle the lock release when processes blocked on it that can now run
 * - if we come here from up_xxxx(), then the RWSEM_FLAG_WAITERS bit must
 *   have been set.
 * - there must be someone on the queue
 * - the wait_lock must be held by the caller
 * - tasks are marked for wakeup, the caller must later invoke wake_up_q()
 *   to actually wakeup the blocked task(s) and drop the reference count,
 *   preferably when the wait_lock is released
 * - woken process blocks are discarded from the list after having task zeroed
 * - writers are only marked woken if downgrading is false
 */
static void __rwsem_mark_wake(struct rw_semaphore *sem,
			      enum rwsem_wake_type wake_type,
			      struct wake_q_head *wake_q)
{
	struct rwsem_waiter *waiter, *tmp;
	long oldcount, woken = 0, adjustment = 0;

	/*
	 * Take a peek at the queue head waiter such that we can determine
	 * the wakeup(s) to perform.
	 */
	waiter = list_first_entry(&sem->wait_list, struct rwsem_waiter, list);

	if (waiter->type == RWSEM_WAITING_FOR_WRITE) {
		if (wake_type == RWSEM_WAKE_ANY) {
			/*
			 * Mark writer at the front of the queue for wakeup.
			 * Until the task is actually later awoken later by
			 * the caller, other writers are able to steal it.
			 * Readers, on the other hand, will block as they
			 * will notice the queued writer.
			 */
			wake_q_add(wake_q, waiter->task);
			lockevent_inc(rwsem_wake_writer);
		}

		return;
	}

	/*
	 * Writers might steal the lock before we grant it to the next reader.
	 * We prefer to do the first reader grant before counting readers
	 * so we can bail out early if a writer stole the lock.
	 */
	if (wake_type != RWSEM_WAKE_READ_OWNED) {
		adjustment = RWSEM_READER_BIAS;
		oldcount = atomic_long_fetch_add(adjustment, &sem->count);
		if (unlikely(oldcount & RWSEM_WRITER_MASK)) {
			/*
			 * Initiate handoff to reader, if applicable.
			 */
			if (!(oldcount & RWSEM_FLAG_HANDOFF) &&
			    time_after(jiffies, waiter->timeout)) {
				adjustment -= RWSEM_FLAG_HANDOFF;
				lockevent_inc(rwsem_rlock_handoff);
			}

			atomic_long_sub(adjustment, &sem->count);
			return;
		}
		/*
		 * Set it to reader-owned to give spinners an early
		 * indication that readers now have the lock.
		 */
		__rwsem_set_reader_owned(sem, waiter->task);
	}

	/*
	 * Grant up to MAX_READERS_WAKEUP read locks to all the readers in the
	 * queue. We know that woken will be at least 1 as we accounted for
	 * above. Note we increment the 'active part' of the count by the
	 * number of readers before waking any processes up.
	 */
	list_for_each_entry_safe(waiter, tmp, &sem->wait_list, list) {
		struct task_struct *tsk;

		if (waiter->type == RWSEM_WAITING_FOR_WRITE)
			continue;

		woken++;
		tsk = waiter->task;

		get_task_struct(tsk);
		list_del(&waiter->list);
		/*
		 * Ensure calling get_task_struct() before setting the reader
		 * waiter to nil such that rwsem_down_read_failed() cannot
		 * race with do_exit() by always holding a reference count
		 * to the task to wakeup.
		 */
		smp_store_release(&waiter->task, NULL);
		/*
		 * Ensure issuing the wakeup (either by us or someone else)
		 * after setting the reader waiter to nil.
		 */
		wake_q_add_safe(wake_q, tsk);

		/*
		 * Limit # of readers that can be woken up per wakeup call.
		 */
		if (woken >= MAX_READERS_WAKEUP)
			break;
	}

	adjustment = woken * RWSEM_READER_BIAS - adjustment;
	lockevent_cond_inc(rwsem_wake_reader, woken);
	if (list_empty(&sem->wait_list)) {
		/* hit end of list above */
		adjustment -= RWSEM_FLAG_WAITERS;
	}

	/*
	 * Clear the handoff flag
	 */
	if (woken && RWSEM_COUNT_HANDOFF(atomic_long_read(&sem->count)))
		adjustment -= RWSEM_FLAG_HANDOFF;

	if (adjustment)
		atomic_long_add(adjustment, &sem->count);
}

/*
 * This function must be called with the sem->wait_lock held to prevent
 * race conditions between checking the rwsem wait list and setting the
 * sem->count accordingly.
 */
static inline bool
rwsem_try_write_lock(long count, struct rw_semaphore *sem, bool first)
{
	long new;

	if (RWSEM_COUNT_LOCKED(count))
		return false;

	if (!first && RWSEM_COUNT_HANDOFF(count))
		return false;

	new = (count & ~RWSEM_FLAG_HANDOFF) + RWSEM_WRITER_LOCKED -
	      (list_is_singular(&sem->wait_list) ? RWSEM_FLAG_WAITERS : 0);

	if (atomic_long_try_cmpxchg_acquire(&sem->count, &count, new)) {
		rwsem_set_owner(sem);
		return true;
	}

	return false;
}

#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
/*
 * Try to acquire read lock before the reader is put on wait queue.
 * Lock acquisition isn't allowed if the rwsem is locked or a writer handoff
 * is ongoing.
 */
static inline bool rwsem_try_read_lock_unqueued(struct rw_semaphore *sem)
{
	long count = atomic_long_read(&sem->count);

	if (RWSEM_COUNT_WLOCKED_OR_HANDOFF(count))
		return false;

	count = atomic_long_fetch_add_acquire(RWSEM_READER_BIAS, &sem->count);
	if (!RWSEM_COUNT_WLOCKED_OR_HANDOFF(count)) {
		rwsem_set_reader_owned(sem);
		lockevent_inc(rwsem_opt_rlock);
		return true;
	}

	/* Back out the change */
	atomic_long_add(-RWSEM_READER_BIAS, &sem->count);
	return false;
}

/*
 * Try to acquire write lock before the writer has been put on wait queue.
 */
static inline bool rwsem_try_write_lock_unqueued(struct rw_semaphore *sem)
{
	long count = atomic_long_read(&sem->count);

	while (!RWSEM_COUNT_LOCKED_OR_HANDOFF(count)) {
		if (atomic_long_try_cmpxchg_acquire(&sem->count, &count,
					count + RWSEM_WRITER_LOCKED)) {
			rwsem_set_owner(sem);
			lockevent_inc(rwsem_opt_wlock);
			return true;
		}
	}
	return false;
}

static inline bool owner_on_cpu(struct task_struct *owner)
{
	/*
	 * As lock holder preemption issue, we both skip spinning if
	 * task is not on cpu or its cpu is preempted
	 */
	return owner->on_cpu && !vcpu_is_preempted(task_cpu(owner));
}

static inline bool rwsem_can_spin_on_owner(struct rw_semaphore *sem)
{
	struct task_struct *owner;
	bool ret = true;

	BUILD_BUG_ON(!rwsem_has_anonymous_owner(RWSEM_OWNER_UNKNOWN));

	if (need_resched()) {
		lockevent_inc(rwsem_opt_fail);
		return false;
	}

	preempt_disable();
	rcu_read_lock();
	owner = rwsem_get_owner(sem);
	if (owner) {
		ret = is_rwsem_owner_spinnable(owner) &&
		     (is_rwsem_owner_reader(owner) || owner_on_cpu(owner));
	}
	rcu_read_unlock();
	preempt_enable();

	lockevent_cond_inc(rwsem_opt_fail, !ret);
	return ret;
}

/*
 * Return the folowing 4 values depending on the lock owner state.
 *   OWNER_NULL  : owner is currently NULL
 *   OWNER_WRITER: when owner changes and is a writer
 *   OWNER_READER: when owner changes and the new owner may be a reader.
 *   OWNER_NONSPINNABLE:
 *		   when optimistic spinning has to stop because either the
 *		   owner stops running, is unknown, or its timeslice has
 *		   been used up.
 */
enum owner_state {
	OWNER_NULL		= 1 << 0,
	OWNER_WRITER		= 1 << 1,
	OWNER_READER		= 1 << 2,
	OWNER_NONSPINNABLE	= 1 << 3,
};
#define OWNER_SPINNABLE		(OWNER_NULL | OWNER_WRITER | OWNER_READER)

static noinline enum owner_state rwsem_spin_on_owner(struct rw_semaphore *sem)
{
	struct task_struct *owner = rwsem_get_owner(sem);
	long count;

	if (!is_rwsem_owner_spinnable(owner))
		return OWNER_NONSPINNABLE;

	rcu_read_lock();
	while (owner && !is_rwsem_owner_reader(owner)) {
		struct task_struct *new_owner = rwsem_get_owner(sem);

		if (new_owner != owner) {
			owner = new_owner;
			break;	/* The owner has changed */
		}

		/*
		 * Ensure we emit the owner->on_cpu, dereference _after_
		 * checking sem->owner still matches owner, if that fails,
		 * owner might point to free()d memory, if it still matches,
		 * the rcu_read_lock() ensures the memory stays valid.
		 */
		barrier();

		/*
		 * abort spinning when need_resched or owner is not running or
		 * owner's cpu is preempted.
		 */
		if (need_resched() || !owner_on_cpu(owner)) {
			rcu_read_unlock();
			return OWNER_NONSPINNABLE;
		}

		cpu_relax();
	}
	rcu_read_unlock();

	/*
	 * If there is a new owner or the owner is not set, we continue
	 * spinning except when here is no active locks and the handoff bit
	 * is set. In this case, we have to stop spinning.
	 */
	if (!is_rwsem_owner_spinnable(owner))
		return OWNER_NONSPINNABLE;
	if (owner && !is_rwsem_owner_reader(owner))
		return OWNER_WRITER;

	count = atomic_long_read(&sem->count);
	if (RWSEM_COUNT_HANDOFF(count) && !RWSEM_COUNT_LOCKED(count))
		return OWNER_NONSPINNABLE;
	return !owner ? OWNER_NULL : OWNER_READER;
}

/*
 * Calculate reader-owned rwsem spinning threshold for writer
 *
 * It is assumed that the more readers own the rwsem, the longer it will
 * take for them to wind down and free the rwsem. So the formula to
 * determine the actual spinning time limit is:
 *
 * 1) RWSEM_FLAG_WAITERS set
 *    Spinning threshold = (10 + nr_readers/2)us
 *
 * 2) RWSEM_FLAG_WAITERS not set
 *    Spinning threshold = 25us
 *
 * In the first case when RWSEM_FLAG_WAITERS is set, no new reader can
 * become rwsem owner. It is assumed that the more readers own the rwsem,
 * the longer it will take for them to wind down and free the rwsem. This
 * is subjected to a maximum value of 25us.
 *
 * In the second case with RWSEM_FLAG_WAITERS off, new readers can join
 * and become one of the owners. So assuming for the worst case and spin
 * for at most 25us.
 */
static inline u64 rwsem_rspin_threshold(struct rw_semaphore *sem)
{
	long count = atomic_long_read(&sem->count);
	int reader_cnt = atomic_long_read(&sem->count) >> RWSEM_READER_SHIFT;

	if (reader_cnt > 30)
		reader_cnt = 30;
	return sched_clock() + ((count & RWSEM_FLAG_WAITERS)
		? 10 * NSEC_PER_USEC + reader_cnt * NSEC_PER_USEC/2
		: 25 * NSEC_PER_USEC);
}

static bool rwsem_optimistic_spin(struct rw_semaphore *sem, bool wlock)
{
	bool taken = false;
	bool is_rt_task = rt_task(current);
	int prev_owner_state = OWNER_NULL;
	int loop = 0;
	u64 rspin_threshold = 0;

	preempt_disable();

	/* sem->wait_lock should not be held when doing optimistic spinning */
	if (!osq_lock(&sem->osq))
		goto done;

	/*
	 * Optimistically spin on the owner field and attempt to acquire the
	 * lock whenever the owner changes. Spinning will be stopped when:
	 *  1) the owning writer isn't running; or
	 *  2) readers own the lock and spinning count has reached 0.
	 */
	for (;;) {
		enum owner_state owner_state = rwsem_spin_on_owner(sem);

		if (!(owner_state & OWNER_SPINNABLE))
			break;

		/*
		 * Try to acquire the lock
		 */
		taken = wlock ? rwsem_try_write_lock_unqueued(sem)
			      : rwsem_try_read_lock_unqueued(sem);

		if (taken)
			break;

		/*
		 * Time-based reader-owned rwsem optimistic spinning
		 */
		if (wlock && (owner_state == OWNER_READER)) {
			/*
			 * Initialize rspin_threshold when the owner
			 * state changes from non-reader to reader.
			 */
			if (prev_owner_state != OWNER_READER) {
				if (!is_rwsem_spinnable(sem))
					break;
				rspin_threshold = rwsem_rspin_threshold(sem);
				loop = 0;
			}

			/*
			 * Check time threshold every 16 iterations to
			 * avoid calling sched_clock() too frequently.
			 * This will make the actual spinning time a
			 * bit more than that specified in the threshold.
			 */
			else if (!(++loop & 0xf) &&
				 (sched_clock() > rspin_threshold)) {
				rwsem_set_nonspinnable(sem);
				lockevent_inc(rwsem_opt_nospin);
				break;
			}
		}

		/*
		 * An RT task cannot do optimistic spinning if it cannot
		 * be sure the lock holder is running or live-lock may
		 * happen if the current task and the lock holder happen
		 * to run in the same CPU.
		 *
		 * When there's no owner or is reader-owned, an RT task
		 * will stop spinning if the owner state is not a writer
		 * at the previous iteration of the loop. This allows the
		 * RT task to recheck if the task that steals the lock is
		 * a spinnable writer. If so, it can keeps on spinning.
		 *
		 * If the owner is a writer, the need_resched() check is
		 * done inside rwsem_spin_on_owner(). If the owner is not
		 * a writer, need_resched() check needs to be done here.
		 */
		if (owner_state != OWNER_WRITER) {
			if (need_resched())
				break;
			if (is_rt_task && (prev_owner_state != OWNER_WRITER))
				break;
		}
		prev_owner_state = owner_state;

		/*
		 * The cpu_relax() call is a compiler barrier which forces
		 * everything in this loop to be re-loaded. We don't need
		 * memory barriers as we'll eventually observe the right
		 * values at the cost of a few extra spins.
		 */
		cpu_relax();
	}
	osq_unlock(&sem->osq);
done:
	preempt_enable();
	lockevent_cond_inc(rwsem_opt_fail, !taken);
	return taken;
}
#else
static inline bool rwsem_can_spin_on_owner(struct rw_semaphore *sem)
{
	return false;
}

static inline bool rwsem_optimistic_spin(struct rw_semaphore *sem, bool wlock)
{
	return false;
}
#endif

/*
 * This is safe to be called without holding the wait_lock.
 */
static inline bool
rwsem_waiter_is_first(struct rw_semaphore *sem, struct rwsem_waiter *waiter)
{
	return list_first_entry(&sem->wait_list, struct rwsem_waiter, list)
			== waiter;
}

/*
 * Wait for the read lock to be granted
 */
static inline struct rw_semaphore __sched *
__rwsem_down_read_failed_common(struct rw_semaphore *sem, int state, long count)
{
	long adjustment = -RWSEM_READER_BIAS;
	struct rwsem_waiter waiter;
	DEFINE_WAKE_Q(wake_q);

	if (unlikely(count < 0)) {
		/*
		 * The sign bit has been set meaning that too many active
		 * readers are present. We need to decrement reader count &
		 * enter wait queue immediately to avoid overflowing the
		 * reader count.
		 *
		 * As preemption is not disabled, there is a remote
		 * possibility that premption can happen in the narrow
		 * timing window between incrementing and decrementing
		 * the reader count and the task is put to sleep for a
		 * considerable amount of time. If sufficient number
		 * of such unfortunate sequence of events happen, we
		 * may still overflow the reader count. It is extremely
		 * unlikey, though. If this is a concern, we should consider
		 * disable preemption during this timing window to make
		 * sure that such unfortunate event will not happen.
		 */
		atomic_long_add(-RWSEM_READER_BIAS, &sem->count);
		adjustment = 0;
		goto queue;
	}

	if (!rwsem_can_spin_on_owner(sem))
		goto queue;

	/*
	 * Undo read bias from down_read() and do optimistic spinning.
	 */
	atomic_long_add(-RWSEM_READER_BIAS, &sem->count);
	adjustment = 0;
	if (rwsem_optimistic_spin(sem, false)) {
		unsigned long flags;

		/*
		 * Opportunistically wake up other readers in the wait queue.
		 * It has another chance of wakeup at unlock time.
		 */
		if ((atomic_long_read(&sem->count) & RWSEM_FLAG_WAITERS) &&
		    raw_spin_trylock_irqsave(&sem->wait_lock, flags)) {
			if (!list_empty(&sem->wait_list))
				__rwsem_mark_wake(sem, RWSEM_WAKE_READ_OWNED,
						  &wake_q);
			raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
			wake_up_q(&wake_q);
		}
		return sem;
	}

queue:
	waiter.task = current;
	waiter.type = RWSEM_WAITING_FOR_READ;
	waiter.timeout = jiffies + RWSEM_WAIT_TIMEOUT;

	raw_spin_lock_irq(&sem->wait_lock);
	if (list_empty(&sem->wait_list)) {
		/*
		 * In case the wait queue is empty and the lock isn't owned
		 * by a writer or has the handoff bit set, this reader can
		 * exit the slowpath and return immediately as its
		 * RWSEM_READER_BIAS has already been set in the count.
		 */
		if (adjustment && !(atomic_long_read(&sem->count) &
		     (RWSEM_WRITER_MASK | RWSEM_FLAG_HANDOFF))) {
			raw_spin_unlock_irq(&sem->wait_lock);
			rwsem_set_reader_owned(sem);
			lockevent_inc(rwsem_rlock_fast);
			return sem;
		}
		adjustment += RWSEM_FLAG_WAITERS;
	}
	list_add_tail(&waiter.list, &sem->wait_list);

	/* we're now waiting on the lock, but no longer actively locking */
	if (adjustment)
		count = atomic_long_add_return(adjustment, &sem->count);
	else
		count = atomic_long_read(&sem->count);

	/*
	 * If there are no active locks, wake the front queued process(es).
	 *
	 * If there are no writers and we are first in the queue,
	 * wake our own waiter to join the existing active readers !
	 */
	if (!RWSEM_COUNT_LOCKED(count) ||
	   (!(count & RWSEM_WRITER_MASK) && (adjustment & RWSEM_FLAG_WAITERS)))
		__rwsem_mark_wake(sem, RWSEM_WAKE_ANY, &wake_q);

	raw_spin_unlock_irq(&sem->wait_lock);
	wake_up_q(&wake_q);

	/* wait to be given the lock */
	while (true) {
		set_current_state(state);
		if (!waiter.task)
			break;
		if (signal_pending_state(state, current)) {
			raw_spin_lock_irq(&sem->wait_lock);
			if (waiter.task)
				goto out_nolock;
			raw_spin_unlock_irq(&sem->wait_lock);
			break;
		}
		schedule();
		lockevent_inc(rwsem_sleep_reader);
	}

	__set_current_state(TASK_RUNNING);
	lockevent_inc(rwsem_rlock);
	return sem;
out_nolock:
	list_del(&waiter.list);
	if (list_empty(&sem->wait_list))
		atomic_long_andnot(RWSEM_FLAG_WAITERS|RWSEM_FLAG_HANDOFF,
				   &sem->count);
	raw_spin_unlock_irq(&sem->wait_lock);
	__set_current_state(TASK_RUNNING);
	lockevent_inc(rwsem_rlock_fail);
	return ERR_PTR(-EINTR);
}

__visible struct rw_semaphore * __sched
rwsem_down_read_failed(struct rw_semaphore *sem, long cnt)
{
	return __rwsem_down_read_failed_common(sem, TASK_UNINTERRUPTIBLE, cnt);
}
EXPORT_SYMBOL(rwsem_down_read_failed);

__visible struct rw_semaphore * __sched
rwsem_down_read_failed_killable(struct rw_semaphore *sem, long cnt)
{
	return __rwsem_down_read_failed_common(sem, TASK_KILLABLE, cnt);
}
EXPORT_SYMBOL(rwsem_down_read_failed_killable);

/*
 * Wait until we successfully acquire the write lock
 */
static inline struct rw_semaphore *
__rwsem_down_write_failed_common(struct rw_semaphore *sem, int state)
{
	long count;
	int first;	/* First one in queue (>1 if handoff set) */
	struct rwsem_waiter waiter;
	struct rw_semaphore *ret = sem;
	DEFINE_WAKE_Q(wake_q);

	/* do optimistic spinning and steal lock if possible */
	if (rwsem_can_spin_on_owner(sem) &&
	    rwsem_optimistic_spin(sem, true))
		return sem;

	/*
	 * Optimistic spinning failed, proceed to the slowpath
	 * and block until we can acquire the sem.
	 */
	waiter.task = current;
	waiter.type = RWSEM_WAITING_FOR_WRITE;
	waiter.timeout = jiffies + RWSEM_WAIT_TIMEOUT;

	raw_spin_lock_irq(&sem->wait_lock);

	/* account for this before adding a new element to the list */
	first = list_empty(&sem->wait_list);

	list_add_tail(&waiter.list, &sem->wait_list);

	/* we're now waiting on the lock */
	if (!first) {
		count = atomic_long_read(&sem->count);

		/*
		 * If there were already threads queued before us and:
		 *  1) there are no no active locks, wake the front
		 *     queued process(es) as the handoff bit might be set.
		 *  2) there are no active writers and some readers, the lock
		 *     must be read owned; so we try to wake any read lock
		 *     waiters that were queued ahead of us.
		 */
		if (!RWSEM_COUNT_LOCKED(count))
			__rwsem_mark_wake(sem, RWSEM_WAKE_ANY, &wake_q);
		else if (!(count & RWSEM_WRITER_MASK) &&
			  (count & RWSEM_READER_MASK))
			__rwsem_mark_wake(sem, RWSEM_WAKE_READERS, &wake_q);
		else
			goto wait;

		/*
		 * The wakeup is normally called _after_ the wait_lock
		 * is released, but given that we are proactively waking
		 * readers we can deal with the wake_q overhead as it is
		 * similar to releasing and taking the wait_lock again
		 * for attempting rwsem_try_write_lock().
		 */
		wake_up_q(&wake_q);

		/*
		 * Reinitialize wake_q after use.
		 */
		wake_q_init(&wake_q);
	} else {
		count = atomic_long_add_return(RWSEM_FLAG_WAITERS, &sem->count);
	}

wait:
	/* wait until we successfully acquire the lock */
	set_current_state(state);
	while (true) {
		if (rwsem_try_write_lock(count, sem, first))
			break;

		raw_spin_unlock_irq(&sem->wait_lock);

		/* Block until there are no active lockers. */
		for (;;) {
			if (signal_pending_state(state, current))
				goto out_nolock;

			schedule();
			lockevent_inc(rwsem_sleep_writer);
			set_current_state(state);
			count = atomic_long_read(&sem->count);

			if (!first)
				first = rwsem_waiter_is_first(sem, &waiter);

			if (!RWSEM_COUNT_LOCKED(count))
				break;

			/*
			 * An RT task sets the HANDOFF bit immediately.
			 * Non-RT task will wait a while before doing so.
			 */
			if (first && !RWSEM_COUNT_HANDOFF(count) &&
			   (rt_task(current) ||
			    time_after(jiffies, waiter.timeout))) {
				atomic_long_or(RWSEM_FLAG_HANDOFF, &sem->count);
				first++;

				/*
				 * Make sure the handoff bit is seen by
				 * others before proceeding.
				 */
				smp_mb__after_atomic();
				lockevent_inc(rwsem_wlock_handoff);
				/*
				 * Do a trylock after setting the handoff
				 * flag to avoid missed wakeup.
				 */
				break;
			}
		}

		raw_spin_lock_irq(&sem->wait_lock);
	}
	__set_current_state(TASK_RUNNING);
	list_del(&waiter.list);
	raw_spin_unlock_irq(&sem->wait_lock);
	lockevent_inc(rwsem_wlock);

	return ret;

out_nolock:
	__set_current_state(TASK_RUNNING);
	raw_spin_lock_irq(&sem->wait_lock);
	list_del(&waiter.list);
	/*
	 * If handoff bit is set by this waiter, make sure that the clearing
	 * of the handoff bit is seen by all before proceeding.
	 */
	if (unlikely(first > 1))
		atomic_long_add_return(-RWSEM_FLAG_HANDOFF,  &sem->count);
	if (list_empty(&sem->wait_list))
		atomic_long_andnot(RWSEM_FLAG_WAITERS, &sem->count);
	else
		__rwsem_mark_wake(sem, RWSEM_WAKE_ANY, &wake_q);
	raw_spin_unlock_irq(&sem->wait_lock);
	wake_up_q(&wake_q);
	lockevent_inc(rwsem_wlock_fail);

	return ERR_PTR(-EINTR);
}

__visible struct rw_semaphore * __sched
rwsem_down_write_failed(struct rw_semaphore *sem)
{
	return __rwsem_down_write_failed_common(sem, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(rwsem_down_write_failed);

__visible struct rw_semaphore * __sched
rwsem_down_write_failed_killable(struct rw_semaphore *sem)
{
	return __rwsem_down_write_failed_common(sem, TASK_KILLABLE);
}
EXPORT_SYMBOL(rwsem_down_write_failed_killable);

/*
 * handle waking up a waiter on the semaphore
 * - up_read/up_write has decremented the active part of count if we come here
 */
__visible
struct rw_semaphore *rwsem_wake(struct rw_semaphore *sem, long count)
{
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	if (!list_empty(&sem->wait_list))
		__rwsem_mark_wake(sem, RWSEM_WAKE_ANY, &wake_q);

	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
	wake_up_q(&wake_q);

	return sem;
}
EXPORT_SYMBOL(rwsem_wake);

/*
 * downgrade a write lock into a read lock
 * - caller incremented waiting part of count and discovered it still negative
 * - just wake up any readers at the front of the queue
 */
__visible
struct rw_semaphore *rwsem_downgrade_wake(struct rw_semaphore *sem)
{
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	if (!list_empty(&sem->wait_list))
		__rwsem_mark_wake(sem, RWSEM_WAKE_READ_OWNED, &wake_q);

	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
	wake_up_q(&wake_q);

	return sem;
}
EXPORT_SYMBOL(rwsem_downgrade_wake);
