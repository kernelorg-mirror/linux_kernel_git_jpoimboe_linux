// SPDX-License-Identifier: GPL-2.0
#include <linux/static_call.h>
#include <linux/memory.h>
#include <linux/bug.h>
#include <asm/text-patching.h>

enum insn_type {
	CALL = 0, /* site call */
	JMP = 1,  /* tramp / site tail-call */
	JCC = 2,
};

/*
 * ud1 %esp, %ecx - a 3 byte #UD that is unique to trampolines, chosen such
 * that there is no false-positive trampoline identification while also being a
 * speculation stop.
 */
static const u8 tramp_ud[] = { 0x0f, 0xb9, 0xcc };

/*
 * cs cs cs xorl %eax, %eax - a single 5 byte instruction that clears %[er]ax
 */
static const u8 xor5rax[] = { 0x2e, 0x2e, 0x2e, 0x31, 0xc0 };

static u8 __is_Jcc(u8 *insn) /* Jcc.d32 */
{
	u8 ret = 0;

	if (insn[0] == 0x0f) {
		u8 tmp = insn[1];
		if ((tmp & 0xf0) == 0x80)
			ret = tmp;
	}

	return ret;
}

static void __ref __static_call_transform(void *insn, enum insn_type type,
					  void *func, bool modinit)
{
	const void *emulate = NULL;
	int size = CALL_INSN_SIZE;
	const void *code;
	u8 op, buf[6];

	if (type == JMP && (op = __is_Jcc(insn)))
		type = JCC;

	switch (type) {
	case CALL:
		func = callthunks_translate_call_dest(func);
		code = text_gen_insn(CALL_INSN_OPCODE, insn, func);
		if (func == &__static_call_return0) {
			emulate = code;
			code = &xor5rax;
		}

		break;

	case JMP:
		code = text_gen_insn(JMP32_INSN_OPCODE, insn, func);
		break;

	case JCC:
		buf[0] = 0x0f;
		__text_gen_insn(buf+1, op, insn+1, func, 5);
		code = buf;
		size = 6;

		break;
	}

	if (memcmp(insn, code, size) == 0)
		return;

	if (system_state == SYSTEM_BOOTING || modinit)
		return text_poke_early(insn, code, size);

	text_poke_bp(insn, code, size, emulate);
}

static void __static_call_validate(u8 *insn, bool tail, bool tramp)
{
	u8 opcode = insn[0];

	if (tramp && memcmp(insn+5, tramp_ud, 3)) {
		pr_err("trampoline signature fail");
		BUG();
	}

	if (tail) {
		if (opcode == JMP32_INSN_OPCODE ||
		    __is_Jcc(insn))
			return;
	} else {
		if (opcode == CALL_INSN_OPCODE ||
		    !memcmp(insn, xor5rax, 5))
			return;
	}

	/*
	 * If we ever trigger this, our text is corrupt, we'll probably not live long.
	 */
	pr_err("unexpected static_call insn opcode 0x%x at %pS\n", opcode, insn);
	BUG();
}

void arch_static_call_transform(void *site, void *tramp, void *func, bool tail)
{
	enum insn_type insn = tail ? JMP : CALL;

	mutex_lock(&text_mutex);

	if (tramp) {
		__static_call_validate(tramp, true, true);
		__static_call_transform(tramp, insn, func, false);
	}

	if (IS_ENABLED(CONFIG_HAVE_STATIC_CALL_INLINE) && site) {
		__static_call_validate(site, tail, false);
		__static_call_transform(site, insn, func, false);
	}

	mutex_unlock(&text_mutex);
}
EXPORT_SYMBOL_GPL(arch_static_call_transform);
