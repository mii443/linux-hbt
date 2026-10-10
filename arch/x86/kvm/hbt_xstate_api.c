// SPDX-License-Identifier: GPL-2.0
/* Versioned storage for software-managed AVX-512 components, per vCPU. */
#include <linux/kvm_host.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "x86.h"
#include "hbt.h"
#include "hbt_xstate.h"

static_assert(sizeof(struct kvm_hbt_xstate) == 1664);
static_assert(offsetof(struct kvm_hbt_xstate, opmask) == 64);
static_assert(KVM_HBT_XSTATE_FEATURES == HBT_XSTATE_AVX512);

void kvm_hbt_xstate_free(struct kvm_vcpu *vcpu)
{
	kfree(vcpu->arch.hbt_xstate);
	vcpu->arch.hbt_xstate = NULL;
}

int kvm_hbt_xstate_validate(struct kvm_hbt_xstate *state)
{
	unsigned int i;

	if (state->version != KVM_HBT_XSTATE_VERSION ||
	    state->size != sizeof(*state) ||
	    state->xfeatures != KVM_HBT_XSTATE_FEATURES ||
	    state->xinuse & ~KVM_HBT_XSTATE_FEATURES)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(state->reserved); i++)
		if (state->reserved[i])
			return -EINVAL;
	if (!(state->xinuse & HBT_XSTATE_OPMASK))
		memset(state->opmask, 0, sizeof(state->opmask));
	if (!(state->xinuse & HBT_XSTATE_ZMM_HI))
		memset(state->zmm_hi, 0, sizeof(state->zmm_hi));
	if (!(state->xinuse & HBT_XSTATE_HI16))
		memset(state->hi16_zmm, 0, sizeof(state->hi16_zmm));
	return 0;
}

void kvm_hbt_xstate_get(struct kvm_vcpu *vcpu, struct kvm_hbt_xstate *state)
{
	if (vcpu->arch.hbt_xstate) {
		*state = *vcpu->arch.hbt_xstate;
	} else {
		memset(state, 0, sizeof(*state));
		state->version = KVM_HBT_XSTATE_VERSION;
		state->size = sizeof(*state);
		state->xfeatures = KVM_HBT_XSTATE_FEATURES;
	}
}

long kvm_hbt_xstate_ioctl(struct kvm_vcpu *vcpu, unsigned int cmd, void __user *argp)
{
	struct kvm_hbt_xstate *state;
	int ret;

	if (!vcpu->kvm->arch.hbt_xstate_storage_enabled ||
	    vcpu->arch.guest_state_protected || is_guest_mode(vcpu))
		return -EOPNOTSUPP;
	if (cmd != KVM_HBT_GET_XSTATE && cmd != KVM_HBT_SET_XSTATE)
		return -ENOTTY;
	state = kzalloc_obj(*state, GFP_KERNEL_ACCOUNT);
	if (!state)
		return -ENOMEM;
	if (cmd == KVM_HBT_GET_XSTATE) {
		kvm_hbt_xstate_get(vcpu, state);
		ret = copy_to_user(argp, state, sizeof(*state)) ? -EFAULT : 0;
	} else {
		ret = -EFAULT;
		if (copy_from_user(state, argp, sizeof(*state)))
			goto out;
		ret = kvm_hbt_xstate_validate(state);
		if (!ret) {
			/* All fallible work precedes publication under the vCPU mutex. */
			kvm_hbt_xstate_free(vcpu);
			vcpu->arch.hbt_xstate = state;
			return 0;
		}
	}
out:
	kfree(state);
	return ret;
}
