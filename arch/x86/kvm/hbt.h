/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_HBT_H
#define __KVM_X86_HBT_H

bool kvm_hbt_prepare_ud(struct kvm_vcpu *vcpu);
long kvm_hbt_ioctl(struct kvm_vcpu *vcpu, unsigned int cmd, void __user *argp);
void kvm_hbt_reset(struct kvm_vcpu *vcpu);

#endif
