/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KVM_VCPU_REGS_H
#define _ASM_X86_KVM_VCPU_REGS_H

#define VCPU_RAX_IDX 0
#define VCPU_RCX_IDX 1
#define VCPU_RDX_IDX 2
#define VCPU_RBX_IDX 3
#define VCPU_RSP_IDX 4
#define VCPU_RBP_IDX 5
#define VCPU_RSI_IDX 6
#define VCPU_RDI_IDX 7

#ifdef CONFIG_X86_64
#define VCPU_R8_IDX  8
#define VCPU_R9_IDX  9
#define VCPU_R10_IDX 10
#define VCPU_R11_IDX 11
#define VCPU_R12_IDX 12
#define VCPU_R13_IDX 13
#define VCPU_R14_IDX 14
#define VCPU_R15_IDX 15
#endif

#endif /* _ASM_X86_KVM_VCPU_REGS_H */

