// SPDX-License-Identifier: GPL-2.0
#include <linux/static_call.h>
#include <linux/memory.h>
#include <linux/bug.h>
#include <asm/text-patching.h>
#include <asm/nospec-branch.h>

#define CALL_INSN_SIZE 5

static inline bool within_cache_line(void *addr, int len)
{
	unsigned long a = (unsigned long)addr;

	return (a >> L1_CACHE_SHIFT) == ((a + len) >> L1_CACHE_SHIFT);
}

void __ref arch_static_call_transform(void *site, void *tramp, void *func)
{
	s32 dest_relative;
	unsigned char opcode;
	void *(*poker)(void *, const void *, size_t);
	void *insn;

	mutex_lock(&text_mutex);

	/*
	 * For x86-64, a 32-bit cross-modifying write to a call destination is
	 * safe as long as it's within a cache line.  In the inline case, if
	 * the call destination is not within a cache line, fall back to using
	 * the out-of-line trampoline.
	 *
	 * We could instead use text_poke_bp() here, which would allow all
	 * static calls to be promoted to inline, but that would require some
	 * trickery to fake a call instruction in the BP handler.
	 */
	if (IS_ENABLED(CONFIG_HAVE_STATIC_CALL_INLINE) &&
	    within_cache_line(site + 1, sizeof(dest_relative)))
		insn = site;
	else
		insn = tramp;

	opcode = *(unsigned char *)insn;
	if (opcode != 0xe8 && opcode != 0xe9) {
		WARN_ONCE(1, "unexpected static call insn opcode 0x%x at %pS",
			  opcode, insn);
		goto done;
	}

	dest_relative = (long)(func) - (long)(insn + CALL_INSN_SIZE);

	poker = early_boot_irqs_disabled ? text_poke_early : text_poke;
	poker(insn + 1, &dest_relative, sizeof(dest_relative));

done:
	mutex_unlock(&text_mutex);
}
EXPORT_SYMBOL_GPL(arch_static_call_transform);
