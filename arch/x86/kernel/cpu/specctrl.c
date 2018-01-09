// SPDX-License-Identifier: GPL-2.0

#include <asm/cpufeature.h>
#include <asm/cpufeatures.h>
#include <asm/nospec-branch.h>

static inline void specctrl_enable_ibrs(void)
{
	setup_force_cpu_cap(X86_FEATURE_IBRS);
}

bool __init specctrl_force_enable_ibrs(void)
{
	if (!boot_cpu_has(X86_FEATURE_SPEC_CTRL))
		return false;
	specctrl_enable_ibrs();
	return true;
}

bool __init specctrl_cond_enable_ibrs(bool full_retpoline)
{
	if (!boot_cpu_has(X86_FEATURE_SPEC_CTRL))
		return false;
	/*
	 * IBRS is only required by SKL or as fallback if retpoline is not
	 * fully supported.
	 */
	if (!is_skylake_era() && full_retpoline)
		return false;

	specctrl_enable_ibrs();
	return true;
}
