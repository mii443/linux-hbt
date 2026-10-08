/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_HBT_H
#define __KVM_X86_HBT_H

bool kvm_hbt_prepare_ud(struct kvm_vcpu *vcpu);
long kvm_hbt_ioctl(struct kvm_vcpu *vcpu, unsigned int cmd, void __user *argp);
void kvm_hbt_reset(struct kvm_vcpu *vcpu);
long kvm_hbt_xstate_ioctl(struct kvm_vcpu *vcpu, unsigned int cmd, void __user *argp);
void kvm_hbt_xstate_free(struct kvm_vcpu *vcpu);
bool kvm_hbt_virtual_xstate_supported(void);
int kvm_hbt_check_cpuid(struct kvm_vcpu *vcpu);
u32 kvm_hbt_xstate_size(struct kvm_vcpu *vcpu, u64 mask);

/* Native KVM XSAVE/FPU buffers always retain the host's layout. */
static inline u64 kvm_hbt_native_xfeatures(struct kvm_vcpu *vcpu, u64 mask)
{
	return vcpu->kvm->arch.hbt_virtual_xstate_enabled ? mask & 7 : mask;
}

#endif
