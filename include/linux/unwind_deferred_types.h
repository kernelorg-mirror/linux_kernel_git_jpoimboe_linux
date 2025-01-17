/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_DEFERRED_TYPES_H
#define _LINUX_UNWIND_USER_DEFERRED_TYPES_H

struct unwind_cache {
	unsigned long		*entries;
	unsigned int		nr_entries;
	u64			cookie;
};

struct unwind_task_info {
	struct unwind_cache	cache;
	u64			cookie;
};

#endif /* _LINUX_UNWIND_USER_DEFERRED_TYPES_H */
