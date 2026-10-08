// SPDX-License-Identifier: GPL-2.0
/* Opt-in virtual XSTATE control plane; execution lives in the emulator. */
#include <linux/kvm_host.h>

#include "cpuid.h"
#include "hbt.h"
#include "hbt_xstate.h"
#include "x86.h"

bool kvm_hbt_virtual_xstate_supported(void)
{
	return kvm_caps.has_hbt_ud &&
	       (kvm_caps.supported_xcr0 & 7) == 7 &&
	       xstate_required_size(7, false) == 832 &&
	       kvm_cpu_cap_has(X86_FEATURE_AVX2);
}

u32 kvm_hbt_xstate_compacted_size(struct kvm_vcpu *vcpu, u64 mask)
{
	struct kvm_cpuid_entry2 *entry;
	u32 size = 576;
	unsigned int i;

	for (i = 2; i < 8; i++) {
		if (!(mask & BIT_ULL(i)))
			continue;
		entry = kvm_find_cpuid_entry_index(vcpu, 0xd, i);
		if (entry) {
			if (entry->ecx & 2)
				size = ALIGN(size, 64);
			size += entry->eax;
		}
	}
	return size;
}

u32 kvm_hbt_xstate_size(struct kvm_vcpu *vcpu, u64 mask)
{
	struct kvm_cpuid_entry2 *entry;
	u32 size = 576;
	unsigned int i;

	for (i = 2; i < 8; i++) {
		if (!(mask & BIT_ULL(i)))
			continue;
		entry = kvm_find_cpuid_entry_index(vcpu, 0xd, i);
		if (entry)
			size = max(size, entry->ebx + entry->eax);
	}
	return size;
}

int kvm_hbt_check_cpuid(struct kvm_vcpu *vcpu)
{
	static const u32 sizes[8] = { 0, 0, 256, 0, 0, 64, 512, 1024 };
	struct hbt_xstate_layout layout = {
		.supported = HBT_XSTATE_SUPPORTED,
		.mxcsr_mask = 0xffff,
	};
	struct kvm_cpuid_entry2 *entry, *base;
	u64 seen = 0;
	unsigned int i;

	if (!vcpu->kvm->arch.hbt_virtual_xstate_enabled)
		return 0;

	/* No nested virtualization, SGX, supervisor state, XFD or dynamic FPU
	 * sizing in v1. Reject before generic CPUID handling has side effects.
	 */
	entry = kvm_find_cpuid_entry(vcpu, 0);
	if (!entry || entry->eax < 0xd)
		return -EINVAL;
	entry = kvm_find_cpuid_entry(vcpu, 1);
	if (!entry || !cpuid_entry_has(entry, X86_FEATURE_XSAVE) ||
	    !cpuid_entry_has(entry, X86_FEATURE_AVX) ||
	    cpuid_entry_has(entry, X86_FEATURE_VMX))
		return -EINVAL;
	entry = kvm_find_cpuid_entry_index(vcpu, 7, 0);
	if (!entry || !cpuid_entry_has(entry, X86_FEATURE_AVX2) ||
	    !cpuid_entry_has(entry, X86_FEATURE_AVX512F) ||
	    cpuid_entry_has(entry, X86_FEATURE_SGX))
		return -EINVAL;
	entry = kvm_find_cpuid_entry(vcpu, 0x80000001);
	if (entry && cpuid_entry_has(entry, X86_FEATURE_SVM))
		return -EINVAL;

	for (i = 0; i < vcpu->arch.cpuid_nent; i++) {
		entry = &vcpu->arch.cpuid_entries[i];
		if (entry->function != 0xd)
			continue;
		if (entry->flags != KVM_CPUID_FLAG_SIGNIFCANT_INDEX ||
		    entry->index >= 64 || (seen & BIT_ULL(entry->index)))
			return -EINVAL;
		seen |= BIT_ULL(entry->index);
		if (entry->index == 0) {
			if (entry->eax != HBT_XSTATE_SUPPORTED || entry->edx)
				return -EINVAL;
		} else if (entry->index == 1) {
			/* Software XSAVEOPT/XSAVEC; EBX is recomputed at runtime. */
			if ((entry->eax & ~3U) || entry->ecx || entry->edx ||
			    (!(entry->eax & 2) && entry->ebx))
				return -EINVAL;
		} else if (entry->index < 8 && sizes[entry->index]) {
			if (entry->eax != sizes[entry->index] ||
			    (entry->ecx & ~2U) || entry->edx)
				return -EINVAL;
			layout.offset[entry->index] = entry->ebx;
			if (entry->ecx & 2)
				layout.align64 |= BIT(entry->index);
		} else if (entry->eax || entry->ebx || entry->ecx || entry->edx) {
			return -EINVAL;
		}
	}
	if ((seen & 0xe7) != 0xe7 || hbt_xstate_layout_valid(&layout))
		return -EINVAL;
	base = kvm_find_cpuid_entry_index(vcpu, 0xd, 0);
	if (base->ecx != kvm_hbt_xstate_size(vcpu, HBT_XSTATE_SUPPORTED))
		return -EINVAL;
	return 0;
}
