// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/static_call.h>
#include <linux/cpu.h>

long __static_call_return0(void)
{
	return 0;
}
EXPORT_SYMBOL_GPL(__static_call_return0);

#if defined(CONFIG_HAVE_STATIC_CALL) && !defined(CONFIG_HAVE_STATIC_CALL_INLINE)
void __static_call_update(struct static_call_key *key, void *tramp, void *func)
{
	cpus_read_lock();
	WRITE_ONCE(key->func, func);
	arch_static_call_transform(NULL, tramp, func, false);
	cpus_read_unlock();
}
EXPORT_SYMBOL_GPL(__static_call_update);
#endif
