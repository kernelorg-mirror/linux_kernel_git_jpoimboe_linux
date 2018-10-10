/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STATIC_CALL_H
#define _LINUX_STATIC_CALL_H

#ifdef CONFIG_HAVE_ARCH_STATIC_CALL
#include <asm/static_call.h>
#else

#define DECLARE_STATIC_CALL(key, func)					\
	extern typeof(func) *key

#define DEFINE_STATIC_CALL(key, func)					\
	typeof(func) *key = func

#define static_call(key, args...)					\
	key(args)

#define static_call_update(key, func)					\
	WRITE_ONCE(key, func)

#define STATIC_CALL_EXPORT(key) EXPORT_SYMBOL(key)
#define STATIC_CALL_EXPORT_GPL(key) EXPORT_SYMBOL_GPL(key)

#endif /* !CONFIG_HAVE_ARCH_STATIC_CALL */

#endif /* _LINUX_STATIC_CALL_H */
