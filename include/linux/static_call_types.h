/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _STATIC_CALL_TYPES_H
#define _STATIC_CALL_TYPES_H

#include <linux/stringify.h>

#define STATIC_CALL_TRAMP_PREFIX ____static_call_tramp_
#define STATIC_CALL_TRAMP_PREFIX_STR __stringify(STATIC_CALL_TRAMP_PREFIX)

#define STATIC_CALL_TRAMP(key) __PASTE(STATIC_CALL_TRAMP_PREFIX, key)
#define STATIC_CALL_TRAMP_STR(key) __stringify(STATIC_CALL_TRAMP(key))

#endif /* _STATIC_CALL_TYPES_H */
