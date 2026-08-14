/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Emit a GNU_PROPERTY_AARCH64_FEATURE_1_BTI note.  This is force-included in
 * every assembly file so the linker emits BTI veneers for >128MB kernels.
 *
 * Clang has -mmark-bti-property, but there's no equivalent for GCC/GAS.
 *
 * Binutils 2.44+ and LLVM 22+ support a much more compact version:
 *
 *   .aeabi_subsection aeabi_feature_and_bits, optional, ULEB128
 *   .aeabi_attribute Tag_Feature_BTI, 1
 */
#ifndef __ASM_BTI_NOTE_H
#define __ASM_BTI_NOTE_H

	.pushsection .note.gnu.property, "a"
	.align	3
	.long	2f - 1f
	.long	6f - 3f
	.long	5		/* NT_GNU_PROPERTY_TYPE_0 */
1:	.string	"GNU"
2:
	.align	3
3:	.long	0xc0000000	/* GNU_PROPERTY_AARCH64_FEATURE_1_AND */
	.long	5f - 4f
4:	.long	1		/* GNU_PROPERTY_AARCH64_FEATURE_1_BTI */
5:
	.align	3
6:
	.popsection

#endif /* __ASM_BTI_NOTE_H */
