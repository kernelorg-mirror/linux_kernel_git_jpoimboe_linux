// SPDX-License-Identifier: GPL-2.0
#include <linux/static_call.h>
#include <linux/memory.h>
#include <linux/bug.h>
#include <asm/text-patching.h>
#include <asm/nospec-branch.h>

#define CALL_INSN_SIZE 5

void __ref arch_static_call_transform(void *site, void *tramp, void *func)
{
	s32 dest_relative;
	unsigned char opcode;
	void *(*poker)(void *, const void *, size_t);
	void *insn = tramp;

	mutex_lock(&text_mutex);

	/*
	 * For x86-64, a 32-bit cross-modifying write to a call destination is
	 * safe as long as it's within a cache line.
	 */
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
