#ifndef _ASM_X86_ATOMIC64_32_H
#define _ASM_X86_ATOMIC64_32_H

#include <linux/compiler.h>
#include <linux/types.h>
//#include <asm/cmpxchg.h>

/* An 64bit atomic type */

typedef struct {
	u64 __aligned(8) counter;
} atomic64_t;

#define ATOMIC64_INIT(val)	{ (val) }

#define __ATOMIC64_DECL(sym) void atomic64_##sym(atomic64_t *, ...)
#ifndef ATOMIC64_EXPORT
#define ATOMIC64_DECL_ONE __ATOMIC64_DECL
#else
#define ATOMIC64_DECL_ONE(sym) __ATOMIC64_DECL(sym); \
	ATOMIC64_EXPORT(atomic64_##sym)
#endif

#ifdef CONFIG_X86_CMPXCHG64
#define __alternative_atomic64(f, g, outputs, inputs, clobbers...)	\
	ASM_CALL("call %P[func]",					\
		 OUTPUTS(outputs),					\
		 INPUTS([func] "i" (atomic64_##g##_cx8)			\
			ARGS_APPEND(inputs)),				\
		 clobbers)

#define ATOMIC64_DECL(sym) ATOMIC64_DECL_ONE(sym##_cx8)
#else
#define __alternative_atomic64(f, g, outputs, inputs, clobbers...)	\
	alternative_call(atomic64_##f##_386, atomic64_##g##_cx8,	\
			 X86_FEATURE_CX8,				\
			 OUTPUTS(outputs), INPUTS(inputs), clobbers)

#define ATOMIC64_DECL(sym) ATOMIC64_DECL_ONE(sym##_cx8); \
	ATOMIC64_DECL_ONE(sym##_386)

ATOMIC64_DECL_ONE(add_386);
ATOMIC64_DECL_ONE(sub_386);
ATOMIC64_DECL_ONE(inc_386);
ATOMIC64_DECL_ONE(dec_386);
#endif

#define alternative_atomic64(f, outputs, inputs, clobbers...)		\
	__alternative_atomic64(f, f, OUTPUTS(outputs), INPUTS(inputs),	\
			       clobbers)

ATOMIC64_DECL(read);
ATOMIC64_DECL(set);
ATOMIC64_DECL(xchg);
ATOMIC64_DECL(add_return);
ATOMIC64_DECL(sub_return);
ATOMIC64_DECL(inc_return);
ATOMIC64_DECL(dec_return);
ATOMIC64_DECL(dec_if_positive);
ATOMIC64_DECL(inc_not_zero);
ATOMIC64_DECL(add_unless);

#undef ATOMIC64_DECL
#undef ATOMIC64_DECL_ONE
#undef __ATOMIC64_DECL
#undef ATOMIC64_EXPORT

/**
 * atomic64_cmpxchg - cmpxchg atomic64 variable
 * @v: pointer to type atomic64_t
 * @o: expected value
 * @n: new value
 *
 * Atomically sets @v to @n if it was equal to @o and returns
 * the old value.
 */

static inline long long atomic64_cmpxchg(atomic64_t *v, long long o, long long n)
{
	return cmpxchg64(&v->counter, o, n);
}

/**
 * atomic64_xchg - xchg atomic64 variable
 * @v: pointer to type atomic64_t
 * @n: value to assign
 *
 * Atomically xchgs the value of @v to @n and returns
 * the old value.
 */
static inline long long atomic64_xchg(atomic64_t *v, long long n)
{
	long long o;
	unsigned high = (unsigned)(n >> 32);
	unsigned low = (unsigned)n;
	alternative_atomic64(xchg,
			     OUTPUTS("=&A" (o)),
			     INPUTS("S" (v), "b" (low), "c" (high)),
			     CLOBBERS("memory"));
	return o;
}

/**
 * atomic64_set - set atomic64 variable
 * @v: pointer to type atomic64_t
 * @i: value to assign
 *
 * Atomically sets the value of @v to @n.
 */
static inline void atomic64_set(atomic64_t *v, long long i)
{
	unsigned high = (unsigned)(i >> 32);
	unsigned low = (unsigned)i;
	alternative_atomic64(set,
			     OUTPUTS(),
			     INPUTS("S" (v), "b" (low), "c" (high)),
			     CLOBBERS("eax", "edx", "memory"));
}

/**
 * atomic64_read - read atomic64 variable
 * @v: pointer to type atomic64_t
 *
 * Atomically reads the value of @v and returns it.
 */
static inline long long atomic64_read(const atomic64_t *v)
{
	long long r;
	alternative_atomic64(read,
			     OUTPUTS("=&A" (r)),
			     INPUTS("c" (v)),
			     CLOBBERS("memory"));
	return r;
 }

/**
 * atomic64_add_return - add and return
 * @i: integer value to add
 * @v: pointer to type atomic64_t
 *
 * Atomically adds @i to @v and returns @i + *@v
 */
static inline long long atomic64_add_return(long long i, atomic64_t *v)
{
	alternative_atomic64(add_return,
			     OUTPUTS("+A" (i), "+c" (v)),
			     INPUTS(),
			     CLOBBERS("memory"));
	return i;
}

/*
 * Other variants with different arithmetic operators:
 */
static inline long long atomic64_sub_return(long long i, atomic64_t *v)
{
	alternative_atomic64(sub_return,
			     OUTPUTS("+A" (i), "+c" (v)),
			     INPUTS(),
			     CLOBBERS("memory"));
	return i;
}

static inline long long atomic64_inc_return(atomic64_t *v)
{
	long long a;
	alternative_atomic64(inc_return,
			     OUTPUTS("=&A" (a)),
			     INPUTS("S" (v)),
			     CLOBBERS("memory", "ecx"));
	return a;
}

static inline long long atomic64_dec_return(atomic64_t *v)
{
	long long a;
	alternative_atomic64(dec_return,
			     OUTPUTS("=&A" (a)),
			     INPUTS("S" (v)),
			     CLOBBERS("memory", "ecx"));
	return a;
}

/**
 * atomic64_add - add integer to atomic64 variable
 * @i: integer value to add
 * @v: pointer to type atomic64_t
 *
 * Atomically adds @i to @v.
 */
static inline long long atomic64_add(long long i, atomic64_t *v)
{
	__alternative_atomic64(add, add_return,
			       OUTPUTS("+A" (i), "+c" (v)),
			       INPUTS(),
			       CLOBBERS("memory"));
	return i;
}

/**
 * atomic64_sub - subtract the atomic64 variable
 * @i: integer value to subtract
 * @v: pointer to type atomic64_t
 *
 * Atomically subtracts @i from @v.
 */
static inline long long atomic64_sub(long long i, atomic64_t *v)
{
	__alternative_atomic64(sub, sub_return,
			       OUTPUTS("+A" (i), "+c" (v)),
			       INPUTS(),
			       CLOBBERS("memory"));
	return i;
}

/**
 * atomic64_sub_and_test - subtract value from variable and test result
 * @i: integer value to subtract
 * @v: pointer to type atomic64_t
 *
 * Atomically subtracts @i from @v and returns
 * true if the result is zero, or false for all
 * other cases.
 */
static inline int atomic64_sub_and_test(long long i, atomic64_t *v)
{
	return atomic64_sub_return(i, v) == 0;
}

/**
 * atomic64_inc - increment atomic64 variable
 * @v: pointer to type atomic64_t
 *
 * Atomically increments @v by 1.
 */
static inline void atomic64_inc(atomic64_t *v)
{
	__alternative_atomic64(inc, inc_return,
			       OUTPUTS(),
			       INPUTS("S" (v)),
			       CLOBBERS("memory", "eax", "ecx", "edx"));
}

/**
 * atomic64_dec - decrement atomic64 variable
 * @v: pointer to type atomic64_t
 *
 * Atomically decrements @v by 1.
 */
static inline void atomic64_dec(atomic64_t *v)
{
	__alternative_atomic64(dec, dec_return,
			       OUTPUTS(),
			       INPUTS("S" (v)),
			       CLOBBERS("memory", "eax", "ecx", "edx"));
}

/**
 * atomic64_dec_and_test - decrement and test
 * @v: pointer to type atomic64_t
 *
 * Atomically decrements @v by 1 and
 * returns true if the result is 0, or false for all other
 * cases.
 */
static inline int atomic64_dec_and_test(atomic64_t *v)
{
	return atomic64_dec_return(v) == 0;
}

/**
 * atomic64_inc_and_test - increment and test
 * @v: pointer to type atomic64_t
 *
 * Atomically increments @v by 1
 * and returns true if the result is zero, or false for all
 * other cases.
 */
static inline int atomic64_inc_and_test(atomic64_t *v)
{
	return atomic64_inc_return(v) == 0;
}

/**
 * atomic64_add_negative - add and test if negative
 * @i: integer value to add
 * @v: pointer to type atomic64_t
 *
 * Atomically adds @i to @v and returns true
 * if the result is negative, or false when
 * result is greater than or equal to zero.
 */
static inline int atomic64_add_negative(long long i, atomic64_t *v)
{
	return atomic64_add_return(i, v) < 0;
}

/**
 * atomic64_add_unless - add unless the number is a given value
 * @v: pointer of type atomic64_t
 * @a: the amount to add to v...
 * @u: ...unless v is equal to u.
 *
 * Atomically adds @a to @v, so long as it was not @u.
 * Returns non-zero if the add was done, zero otherwise.
 */
static inline int atomic64_add_unless(atomic64_t *v, long long a, long long u)
{
	unsigned low = (unsigned)u;
	unsigned high = (unsigned)(u >> 32);
	alternative_atomic64(add_unless,
			     OUTPUTS("+A" (a), "+c" (low), "+D" (high)),
			     INPUTS("S" (v)),
			     CLOBBERS("memory"));
	return (int)a;
}


static inline int atomic64_inc_not_zero(atomic64_t *v)
{
	int r;
	alternative_atomic64(inc_not_zero,
			     OUTPUTS("=&a" (r)),
			     INPUTS("S" (v)),
			     CLOBBERS("ecx", "edx", "memory"));
	return r;
}

static inline long long atomic64_dec_if_positive(atomic64_t *v)
{
	long long r;
	alternative_atomic64(dec_if_positive,
			     OUTPUTS("=&A" (r)),
			     INPUTS("S" (v)),
			     CLOBBERS("ecx", "memory"));
	return r;
}

#undef alternative_atomic64
#undef __alternative_atomic64

static inline void atomic64_and(long long i, atomic64_t *v)
{
	long long old, c = 0;

	while ((old = atomic64_cmpxchg(v, c, c & i)) != c)
		c = old;
}

static inline long long atomic64_fetch_and(long long i, atomic64_t *v)
{
	long long old, c = 0;

	while ((old = atomic64_cmpxchg(v, c, c & i)) != c)
		c = old;

	return old;
}

static inline void atomic64_or(long long i, atomic64_t *v)
{
	long long old, c = 0;

	while ((old = atomic64_cmpxchg(v, c, c | i)) != c)
		c = old;
}

static inline long long atomic64_fetch_or(long long i, atomic64_t *v)
{
	long long old, c = 0;

	while ((old = atomic64_cmpxchg(v, c, c | i)) != c)
		c = old;

	return old;
}

static inline void atomic64_xor(long long i, atomic64_t *v)
{
	long long old, c = 0;

	while ((old = atomic64_cmpxchg(v, c, c ^ i)) != c)
		c = old;
}

static inline long long atomic64_fetch_xor(long long i, atomic64_t *v)
{
	long long old, c = 0;

	while ((old = atomic64_cmpxchg(v, c, c ^ i)) != c)
		c = old;

	return old;
}

static inline long long atomic64_fetch_add(long long i, atomic64_t *v)
{
	long long old, c = 0;

	while ((old = atomic64_cmpxchg(v, c, c + i)) != c)
		c = old;

	return old;
}

#define atomic64_fetch_sub(i, v)	atomic64_fetch_add(-(i), (v))

#endif /* _ASM_X86_ATOMIC64_32_H */
