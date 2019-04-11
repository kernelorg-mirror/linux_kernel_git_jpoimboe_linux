/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The least significant 2 bits of the owner value has the following
 * meanings when set.
 *  - RWSEM_READER_OWNED (bit 0): The rwsem is owned by readers
 *  - RWSEM_ANONYMOUSLY_OWNED (bit 1): The rwsem is anonymously owned,
 *    i.e. the owner(s) cannot be readily determined. It can be reader
 *    owned or the owning writer is indeterminate. Optimistic spinning
 *    should be disabled if this flag is set.
 *
 * When a writer acquires a rwsem, it puts its task_struct pointer
 * into the owner field or the count itself (64-bit only. It should
 * be cleared after an unlock.
 *
 * When a reader acquires a rwsem, it will also puts its task_struct
 * pointer into the owner field with the RWSEM_READER_OWNED bit set.
 * On unlock, the owner field will largely be left untouched. So
 * for a free or reader-owned rwsem, the owner value may contain
 * information about the last reader that acquires the rwsem. The
 * anonymous bit may also be set to permanently disable optimistic
 * spinning on a reader-own rwsem until a writer comes along.
 *
 * That information may be helpful in debugging cases where the system
 * seems to hang on a reader owned rwsem especially if only one reader
 * is involved. Ideally we would like to track all the readers that own
 * a rwsem, but the overhead is simply too big.
 */
#include "lock_events.h"

#define RWSEM_READER_OWNED	(1UL << 0)
#define RWSEM_ANONYMOUSLY_OWNED	(1UL << 1)

#ifdef CONFIG_DEBUG_RWSEMS
# define DEBUG_RWSEMS_WARN_ON(c, sem)	do {			\
	if (!debug_locks_silent &&				\
	    WARN_ONCE(c, "DEBUG_RWSEMS_WARN_ON(%s): count = 0x%lx, owner = 0x%lx, curr 0x%lx, list %sempty\n",\
		#c, atomic_long_read(&(sem)->count),		\
		(long)((sem)->owner), (long)current,		\
		list_empty(&(sem)->wait_list) ? "" : "not "))	\
			debug_locks_off();			\
	} while (0)
#else
# define DEBUG_RWSEMS_WARN_ON(c, sem)
#endif

/*
 * Enable the merging of owner into count for x86-64 only.
 */
#ifdef CONFIG_X86_64
#define RWSEM_MERGE_OWNER_TO_COUNT
#endif

/*
 * With separate count and owner, there are timing windows where the two
 * values are inconsistent. That can cause problem when trying to figure
 * out the exact state of the rwsem. That can be solved by combining
 * the count and owner together in a single atomic value.
 *
 * On 64-bit architectures, the owner task structure pointer can be
 * compressed and combined with reader count and other status flags.
 * A simple compression method is to map the virtual address back to
 * the physical address by subtracting PAGE_OFFSET. On 32-bit
 * architectures, the long integer value just isn't big enough for
 * combining owner and count. So they remain separate.
 *
 * For x86-64, the physical address can use up to 52 bits. That is 4PB
 * of memory. That leaves 12 bits available for other use. The task
 * structure pointer is also aligned to the L1 cache size. That means
 * another 6 bits (64 bytes cacheline) will be available. Reserving
 * 2 bits for status flags, we will have 16 bits for the reader count
 * and read fail bit. That can supports up to (32k-1) active readers.
 *
 * On x86-64, the bit definitions of the count are:
 *
 * Bit   0    - waiters present bit
 * Bit   1    - lock handoff bit
 * Bits  2-47 - compressed task structure pointer
 * Bits 48-62 - 15-bit reader counts
 * Bit  63    - read fail bit
 *
 * On other 64-bit architectures, the bit definitions are:
 *
 * Bit  0    - writer locked bit
 * Bit  1    - waiters present bit
 * Bit  2    - lock handoff bit
 * Bits 3-7  - reserved
 * Bits 8-62 - 55-bit reader count
 * Bit  63   - read fail bit
 *
 * On 32-bit architectures, the bit definitions of the count are:
 *
 * Bit  0    - writer locked bit
 * Bit  1    - waiters present bit
 * Bit  2    - lock handoff bit
 * Bits 3-7  - reserved
 * Bits 8-30 - 23-bit reader count
 * Bit  31   - read fail bit
 *
 * It is not likely that the most significant bit (read fail bit) will ever
 * be set. This guard bit is still checked anyway in the down_read() fastpath
 * just in case we need to use up more of the reader bits for other purpose
 * in the future.
 *
 * atomic_long_fetch_add() is used to obtain reader lock, whereas
 * atomic_long_cmpxchg() will be used to obtain writer lock.
 */
#define RWSEM_FLAG_WAITERS	(1UL << 0)
#define RWSEM_FLAG_HANDOFF	(1UL << 1)
#define RWSEM_FLAG_READFAIL	(1UL << (BITS_PER_LONG - 1))


#ifdef RWSEM_MERGE_OWNER_TO_COUNT

#ifdef __PHYSICAL_MASK_SHIFT
#define RWSEM_PA_MASK_SHIFT	__PHYSICAL_MASK_SHIFT
#else
#define RWSEM_PA_MASK_SHIFT	52
#endif
#define RWSEM_READER_SHIFT	(RWSEM_PA_MASK_SHIFT - L1_CACHE_SHIFT + 2)
#define RWSEM_WRITER_MASK	((1UL << RWSEM_READER_SHIFT) - 4)
#define RWSEM_WRITER_LOCKED	rwsem_owner_count(current)

#else /* RWSEM_MERGE_OWNER_TO_COUNT */
#define RWSEM_READER_SHIFT	8
#define RWSEM_WRITER_MASK	(1UL << 7)
#define RWSEM_WRITER_LOCKED	RWSEM_WRITER_MASK
#endif /* RWSEM_MERGE_OWNER_TO_COUNT */

#define RWSEM_READER_BIAS	(1UL << RWSEM_READER_SHIFT)
#define RWSEM_READER_MASK	(~(RWSEM_READER_BIAS - 1))
#define RWSEM_LOCK_MASK		(RWSEM_WRITER_MASK|RWSEM_READER_MASK)
#define RWSEM_READ_FAILED_MASK	(RWSEM_WRITER_MASK|RWSEM_FLAG_WAITERS|\
				 RWSEM_FLAG_HANDOFF|RWSEM_FLAG_READFAIL)

#define RWSEM_COUNT_LOCKED(c)	((c) & RWSEM_LOCK_MASK)
#define RWSEM_COUNT_WLOCKED(c)	((c) & RWSEM_WRITER_MASK)
#define RWSEM_COUNT_HANDOFF(c)	((c) & RWSEM_FLAG_HANDOFF)
#define RWSEM_COUNT_LOCKED_OR_HANDOFF(c)	\
	((c) & (RWSEM_LOCK_MASK|RWSEM_FLAG_HANDOFF))
#define RWSEM_COUNT_WLOCKED_OR_HANDOFF(c)	\
	((c) & (RWSEM_WRITER_MASK | RWSEM_FLAG_HANDOFF))

/*
 * Task structure pointer compression (64-bit only):
 * (owner - PAGE_OFFSET) >> (L1_CACHE_SHIFT - 2)
 */
static inline unsigned long rwsem_owner_count(struct task_struct *owner)
{
	return ((unsigned long)owner - PAGE_OFFSET) >> (L1_CACHE_SHIFT - 2);
}

static inline unsigned long rwsem_count_owner(long count)
{
	unsigned long writer = (unsigned long)count & RWSEM_WRITER_MASK;

	return writer ? (writer << (L1_CACHE_SHIFT - 2)) + PAGE_OFFSET : 0;
}

/*
 * All writes to owner are protected by WRITE_ONCE() to make sure that
 * store tearing can't happen as optimistic spinners may read and use
 * the owner value concurrently without lock. Read from owner, however,
 * may not need READ_ONCE() as long as the pointer value is only used
 * for comparison and isn't being dereferenced.
 *
 * On 32-bit architectures, the owner and count are separate. On 64-bit
 * architectures, however, the writer task structure pointer is written
 * to the count as well in addition to the owner field.
 */

static inline void rwsem_set_owner(struct rw_semaphore *sem)
{
	WRITE_ONCE(sem->owner, current);
}

static inline void rwsem_clear_owner(struct rw_semaphore *sem)
{
	WRITE_ONCE(sem->owner, NULL);
}

#ifdef RWSEM_MERGE_OWNER_TO_COUNT
/*
 * Get the owner value from count to have early access to the task structure.
 * Owner from sem->count should includes the RWSEM_ANONYMOUSLY_OWNED bit
 * from sem->owner.
 */
static inline struct task_struct *rwsem_get_owner(struct rw_semaphore *sem)
{
	unsigned long cowner = rwsem_count_owner(atomic_long_read(&sem->count));
	unsigned long sowner = (unsigned long)READ_ONCE(sem->owner);

	return (struct task_struct *) (cowner
		? cowner | (sowner & RWSEM_ANONYMOUSLY_OWNED) : sowner);
}
#else /* !RWSEM_MERGE_OWNER_TO_COUNT */
static inline struct task_struct *rwsem_get_owner(struct rw_semaphore *sem)
{
	return READ_ONCE(sem->owner);
}
#endif /* RWSEM_MERGE_OWNER_TO_COUNT */

/*
 * The task_struct pointer of the last owning reader will be left in
 * the owner field.
 *
 * Note that the owner value just indicates the task has owned the rwsem
 * previously, it may not be the real owner or one of the real owners
 * anymore when that field is examined, so take it with a grain of salt.
 */
static inline void __rwsem_set_reader_owned(struct rw_semaphore *sem,
					    struct task_struct *owner)
{
	unsigned long val = (unsigned long)owner | RWSEM_READER_OWNED;

	WRITE_ONCE(sem->owner, (struct task_struct *)val);
}

static inline void rwsem_set_reader_owned(struct rw_semaphore *sem)
{
	__rwsem_set_reader_owned(sem, current);
}

/*
 * Return true if the a rwsem waiter can spin on the rwsem's owner
 * and steal the lock, i.e. the lock is not anonymously owned.
 * N.B. !owner is considered spinnable.
 */
static inline bool is_rwsem_owner_spinnable(struct task_struct *owner)
{
	return !((unsigned long)owner & RWSEM_ANONYMOUSLY_OWNED);
}

static inline bool is_rwsem_owner_reader(struct task_struct *owner)
{
	return (unsigned long)owner & RWSEM_READER_OWNED;
}

/*
 * Return true if the rwsem is spinnable.
 */
static inline bool is_rwsem_spinnable(struct rw_semaphore *sem)
{
	return is_rwsem_owner_spinnable(READ_ONCE(sem->owner));
}

/*
 * Return true if the rwsem is owned by a reader.
 */
static inline bool is_rwsem_reader_owned(struct rw_semaphore *sem)
{
#ifdef CONFIG_DEBUG_RWSEMS
	/*
	 * Check the count to see if it is write-locked.
	 */
	long count = atomic_long_read(&sem->count);

	if (count & RWSEM_WRITER_MASK)
		return false;
#endif
	return (unsigned long)sem->owner & RWSEM_READER_OWNED;
}

/*
 * Return true if rwsem is owned by an anonymous writer or readers.
 */
static inline bool rwsem_has_anonymous_owner(struct task_struct *owner)
{
	return (unsigned long)owner & RWSEM_ANONYMOUSLY_OWNED;
}

#ifdef CONFIG_DEBUG_RWSEMS
/*
 * With CONFIG_DEBUG_RWSEMS configured, it will make sure that if there
 * is a task pointer in owner of a reader-owned rwsem, it will be the
 * real owner or one of the real owners. The only exception is when the
 * unlock is done by up_read_non_owner().
 */
static inline void rwsem_clear_reader_owned(struct rw_semaphore *sem)
{
	unsigned long val = (unsigned long)current | RWSEM_READER_OWNED
						   | RWSEM_ANONYMOUSLY_OWNED;

	if (READ_ONCE(sem->owner) == (struct task_struct *)val)
		cmpxchg_relaxed((unsigned long *)&sem->owner, val,
				RWSEM_READER_OWNED | RWSEM_ANONYMOUSLY_OWNED);
}
#else
static inline void rwsem_clear_reader_owned(struct rw_semaphore *sem)
{
}
#endif

extern struct rw_semaphore *
rwsem_down_read_failed(struct rw_semaphore *sem, long count);
extern struct rw_semaphore *
rwsem_down_read_failed_killable(struct rw_semaphore *sem, long count);
extern struct rw_semaphore *
rwsem_down_write_failed(struct rw_semaphore *sem);
extern struct rw_semaphore *
rwsem_down_write_failed_killable(struct rw_semaphore *sem);

extern struct rw_semaphore *rwsem_wake(struct rw_semaphore *sem, long count);
extern struct rw_semaphore *rwsem_downgrade_wake(struct rw_semaphore *sem);

/*
 * Set the RWSEM_ANONYMOUSLY_OWNED flag if the RWSEM_READER_OWNED flag
 * remains set. Otherwise, the operation will be aborted.
 */
static inline void rwsem_set_nonspinnable(struct rw_semaphore *sem)
{
	long owner = (long)READ_ONCE(sem->owner);

	while (is_rwsem_owner_reader((struct task_struct *)owner)) {
		if (!is_rwsem_owner_spinnable((struct task_struct *)owner))
			break;
		owner = cmpxchg((long *)&sem->owner, owner,
				owner | RWSEM_ANONYMOUSLY_OWNED);
	}
}

/*
 * lock for reading
 */
static inline void __down_read(struct rw_semaphore *sem)
{
	long count = atomic_long_fetch_add_acquire(RWSEM_READER_BIAS,
						   &sem->count);

	if (unlikely(count & RWSEM_READ_FAILED_MASK)) {
		rwsem_down_read_failed(sem, count);
		DEBUG_RWSEMS_WARN_ON(!is_rwsem_reader_owned(sem), sem);
	} else {
		rwsem_set_reader_owned(sem);
	}
}

static inline int __down_read_killable(struct rw_semaphore *sem)
{
	long count = atomic_long_fetch_add_acquire(RWSEM_READER_BIAS,
						   &sem->count);

	if (unlikely(count & RWSEM_READ_FAILED_MASK)) {
		if (IS_ERR(rwsem_down_read_failed_killable(sem, count)))
			return -EINTR;
		DEBUG_RWSEMS_WARN_ON(!is_rwsem_reader_owned(sem), sem);
	} else {
		rwsem_set_reader_owned(sem);
	}
	return 0;
}

static inline int __down_read_trylock(struct rw_semaphore *sem)
{
	/*
	 * Optimize for the case when the rwsem is not locked at all.
	 */
	long tmp = RWSEM_UNLOCKED_VALUE;

	lockevent_inc(rwsem_rtrylock);
	do {
		if (atomic_long_try_cmpxchg_acquire(&sem->count, &tmp,
					tmp + RWSEM_READER_BIAS)) {
			rwsem_set_reader_owned(sem);
			return 1;
		}
	} while (!(tmp & RWSEM_READ_FAILED_MASK));
	return 0;
}

/*
 * lock for writing
 */
static inline void __down_write(struct rw_semaphore *sem)
{
	if (unlikely(atomic_long_cmpxchg_acquire(&sem->count, 0,
						 RWSEM_WRITER_LOCKED)))
		rwsem_down_write_failed(sem);
	rwsem_set_owner(sem);
#ifdef RWSEM_MERGE_OWNER_TO_COUNT
	DEBUG_RWSEMS_WARN_ON(sem->owner != rwsem_get_owner(sem), sem);
#endif
}

static inline int __down_write_killable(struct rw_semaphore *sem)
{
	if (unlikely(atomic_long_cmpxchg_acquire(&sem->count, 0,
						 RWSEM_WRITER_LOCKED)))
		if (IS_ERR(rwsem_down_write_failed_killable(sem)))
			return -EINTR;
	rwsem_set_owner(sem);
	return 0;
}

static inline int __down_write_trylock(struct rw_semaphore *sem)
{
	long tmp;

	lockevent_inc(rwsem_wtrylock);
	tmp = atomic_long_cmpxchg_acquire(&sem->count, RWSEM_UNLOCKED_VALUE,
					  RWSEM_WRITER_LOCKED);
	if (tmp == RWSEM_UNLOCKED_VALUE) {
		rwsem_set_owner(sem);
		return true;
	}
	return false;
}

/*
 * unlock after reading
 */
static inline void __up_read(struct rw_semaphore *sem)
{
	long tmp;

	DEBUG_RWSEMS_WARN_ON(!is_rwsem_reader_owned(sem), sem);
	rwsem_clear_reader_owned(sem);
	tmp = atomic_long_add_return_release(-RWSEM_READER_BIAS, &sem->count);
	if (unlikely((tmp & (RWSEM_LOCK_MASK|RWSEM_FLAG_WAITERS))
			== RWSEM_FLAG_WAITERS))
		rwsem_wake(sem, tmp);
}

/*
 * unlock after writing
 */
static inline void __up_write(struct rw_semaphore *sem)
{
	long tmp;

	DEBUG_RWSEMS_WARN_ON(sem->owner != current, sem);
	rwsem_clear_owner(sem);
	tmp = atomic_long_fetch_and_release(~RWSEM_WRITER_MASK, &sem->count);
	if (unlikely(tmp & RWSEM_FLAG_WAITERS))
		rwsem_wake(sem, tmp);
}

/*
 * downgrade write lock to read lock
 */
static inline void __downgrade_write(struct rw_semaphore *sem)
{
	long tmp;

	/*
	 * When downgrading from exclusive to shared ownership,
	 * anything inside the write-locked region cannot leak
	 * into the read side. In contrast, anything in the
	 * read-locked region is ok to be re-ordered into the
	 * write side. As such, rely on RELEASE semantics.
	 */
	DEBUG_RWSEMS_WARN_ON(sem->owner != current, sem);
	tmp = atomic_long_fetch_add_release(
		-RWSEM_WRITER_LOCKED+RWSEM_READER_BIAS, &sem->count);
	rwsem_set_reader_owned(sem);
	if (tmp & RWSEM_FLAG_WAITERS)
		rwsem_downgrade_wake(sem);
}
