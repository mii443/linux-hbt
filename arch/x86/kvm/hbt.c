// SPDX-License-Identifier: GPL-2.0
/* #UD analysis, single-vCPU retry and XOM commit. Decoding stays in userspace. */
#include <linux/kvm_host.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "x86.h"
#include "hbt.h"
#include "mmu.h"

struct kvm_hbt_state {
	struct kvm_hbt_snapshot snapshot;
	u64 cr0, cr4, efer;
	bool acknowledged;
	bool retry;
};

static unsigned int kvm_hbt_mode(struct kvm_vcpu *vcpu)
{
	int cs_db, cs_l;

	kvm_x86_call(get_cs_db_l_bits)(vcpu, &cs_db, &cs_l);
	if (is_long_mode(vcpu) && cs_l)
		return 64;
	if (is_protmode(vcpu) && cs_db &&
	    !(kvm_get_rflags(vcpu) & X86_EFLAGS_VM))
		return 32;
	return 0;
}

static bool kvm_hbt_same_context(struct kvm_vcpu *vcpu,
				 struct kvm_hbt_state *state)
{
	struct kvm_hbt_snapshot *s = &state->snapshot;

	return kvm_rip_read(vcpu) == s->rip &&
		kvm_get_linear_rip(vcpu) == s->linear_rip &&
		kvm_read_cr3(vcpu) == s->cr3 &&
		kvm_read_cr0(vcpu) == state->cr0 &&
		kvm_read_cr4(vcpu) == state->cr4 &&
		vcpu->arch.efer == state->efer &&
		kvm_hbt_mode(vcpu) == s->mode &&
		!is_guest_mode(vcpu) && !vcpu->arch.guest_state_protected &&
		!vcpu->arch.exception.pending && !vcpu->arch.exception.injected;
}

static int kvm_hbt_finish(struct kvm_vcpu *vcpu)
{
	struct kvm_hbt_state *state = vcpu->arch.hbt;
	bool same_context, retry;

	if (!state)
		return 1;
	same_context = kvm_hbt_same_context(vcpu, state);
	retry = state->acknowledged && state->retry;
	vcpu->arch.hbt = NULL;
	kfree(state);

	/*
	 * Missing completion also falls back. Call the normal #UD handler
	 * directly, so the same fault is not submitted to userspace again.
	 * Userspace may have replaced the vCPU context while it was stopped.
	 */
	return same_context && !retry ? handle_ud(vcpu) : 1;
}

void kvm_hbt_reset(struct kvm_vcpu *vcpu)
{
	if (vcpu->arch.complete_userspace_io == kvm_hbt_finish)
		vcpu->arch.complete_userspace_io = NULL;
	kfree(vcpu->arch.hbt);
	vcpu->arch.hbt = NULL;
	/* Do not reuse request IDs across RESET/INIT. */
}

bool kvm_hbt_prepare_ud(struct kvm_vcpu *vcpu)
{
	struct x86_exception exception = {};
	struct kvm_hbt_state *state;
	struct kvm_hbt_snapshot *s;
	unsigned int mode;
	unsigned long linear;
	gpa_t gpa;

	if (!vcpu->kvm->arch.hbt_ud_enabled || is_guest_mode(vcpu) ||
	    vcpu->arch.guest_state_protected ||
	    vcpu->arch.complete_userspace_io || vcpu->arch.hbt)
		return false;
	/* The automatic EPTP experiment instruments ordinary user code only. */
	if (vcpu->kvm->arch.hbt_virtual_xstate_enabled &&
	    kvm_x86_call(get_cpl)(vcpu) != 3)
		return false;
	mode = kvm_hbt_mode(vcpu);
	if (!mode)
		return false;
	linear = kvm_get_linear_rip(vcpu);
	gpa = kvm_mmu_gva_to_gpa_fetch(vcpu, linear, &exception);
	if (gpa == INVALID_GPA)
		return false;
	state = kzalloc_obj(*state, GFP_KERNEL_ACCOUNT);
	if (!state)
		return false;
	s = &state->snapshot;
	s->version = KVM_HBT_ABI_VERSION;
	s->rip = kvm_rip_read(vcpu);
	s->linear_rip = linear;
	s->gpa = gpa;
	s->cr3 = kvm_read_cr3(vcpu);
	s->mode = mode;
	s->data_len = KVM_HBT_MAX_BYTES - offset_in_page(linear);
	if (kvm_vcpu_read_xom_guest(vcpu, gpa, s->data, s->data_len)) {
		kfree(state);
		return false;
	}
	state->cr0 = kvm_read_cr0(vcpu);
	state->cr4 = kvm_read_cr4(vcpu);
	state->efer = vcpu->arch.efer;
	if (!++vcpu->arch.hbt_request_id)
		++vcpu->arch.hbt_request_id;
	s->request_id = vcpu->arch.hbt_request_id;
	vcpu->arch.hbt = state;
	vcpu->arch.complete_userspace_io = kvm_hbt_finish;
	vcpu->run->exit_reason = KVM_EXIT_HBT_X86_UD;
	vcpu->run->hbt.version = KVM_HBT_ABI_VERSION;
	vcpu->run->hbt.reserved = 0;
	vcpu->run->hbt.request_id = s->request_id;
	return true;
}
EXPORT_SYMBOL_FOR_KVM_INTERNAL(kvm_hbt_prepare_ud);

static int kvm_hbt_install_xom(struct kvm_vcpu *vcpu,
			       struct kvm_hbt_state *state, void __user *argp)
{
	struct kvm_hbt_xom_install req;
	struct x86_exception exception = {};
	struct kvm_hbt_snapshot *s = &state->snapshot;
	u8 *images;
	int ret;

	if (!vcpu->kvm->arch.hbt_xom_enabled)
		return -EOPNOTSUPP;
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (req.version != KVM_HBT_ABI_VERSION || req.reserved ||
	    offset_in_page(req.gpa) || req.gpa != (s->gpa & PAGE_MASK))
		return -EINVAL;
	if (req.request_id != s->request_id)
		return -ESTALE;
	if (state->acknowledged)
		return -EALREADY;
	if (!kvm_hbt_same_context(vcpu, state) ||
	    kvm_mmu_gva_to_gpa_fetch(vcpu, s->linear_rip, &exception) != s->gpa)
		return -ESTALE;

	images = kmalloc(2 * PAGE_SIZE, GFP_KERNEL_ACCOUNT);
	if (!images)
		return -ENOMEM;
	ret = -EFAULT;
	if (copy_from_user(images, u64_to_user_ptr(req.original_addr), PAGE_SIZE) ||
	    copy_from_user(images + PAGE_SIZE,
			   u64_to_user_ptr(req.replacement_addr), PAGE_SIZE))
		goto out;
	ret = -ESTALE;
	if (memcmp(images + offset_in_page(s->gpa), s->data, s->data_len))
		goto out;
	ret = kvm_install_xom_page(vcpu, req.gpa, images, images + PAGE_SIZE);
	if (!ret) {
		state->retry = true;
		state->acknowledged = true;
	}
out:
	kfree(images);
	return ret;
}

static int kvm_hbt_xom_request(struct kvm_vcpu *vcpu, struct kvm_hbt_state *state,
			       u32 version, u32 reserved, u64 id, gpa_t gpa)
{
	struct x86_exception exception = {};
	struct kvm_hbt_snapshot *s = &state->snapshot;

	if (version != KVM_HBT_ABI_VERSION || reserved || offset_in_page(gpa) ||
	    gpa != (s->gpa & PAGE_MASK))
		return -EINVAL;
	if (id != s->request_id || !kvm_hbt_same_context(vcpu, state) ||
	    kvm_mmu_gva_to_gpa_fetch(vcpu, s->linear_rip, &exception) != s->gpa)
		return -ESTALE;
	return state->acknowledged ? -EALREADY : 0;
}

static int kvm_hbt_get_xom_page(struct kvm_vcpu *vcpu,
				struct kvm_hbt_state *state, void __user *argp)
{
	struct kvm_hbt_xom_page req;
	u8 *images;
	int ret;

	if (!vcpu->kvm->arch.hbt_xom_enabled)
		return -EOPNOTSUPP;
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (req.generation)
		return -EINVAL;
	ret = kvm_hbt_xom_request(vcpu, state, req.version, req.reserved,
				  req.request_id, req.gpa);
	if (ret)
		return ret;
	images = kmalloc(2 * PAGE_SIZE, GFP_KERNEL_ACCOUNT);
	if (!images)
		return -ENOMEM;
	ret = kvm_get_xom_page(vcpu, req.gpa, images, images + PAGE_SIZE, &req.generation);
	if (!ret && (copy_to_user(u64_to_user_ptr(req.original_addr), images, PAGE_SIZE) ||
		     copy_to_user(u64_to_user_ptr(req.current_addr),
				  images + PAGE_SIZE, PAGE_SIZE) ||
		     copy_to_user(argp, &req, sizeof(req))))
		ret = -EFAULT;
	kfree(images);
	return ret;
}

static int kvm_hbt_update_xom(struct kvm_vcpu *vcpu,
			      struct kvm_hbt_state *state, void __user *argp)
{
	struct kvm_hbt_xom_update req;
	struct kvm_hbt_snapshot *s = &state->snapshot;
	u8 *images;
	int ret;

	if (!vcpu->kvm->arch.hbt_xom_enabled)
		return -EOPNOTSUPP;
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	ret = kvm_hbt_xom_request(vcpu, state, req.version, req.reserved,
				  req.request_id, req.gpa);
	if (ret)
		return ret;
	images = kmalloc(3 * PAGE_SIZE, GFP_KERNEL_ACCOUNT);
	if (!images)
		return -ENOMEM;
	ret = -EFAULT;
	if (copy_from_user(images, u64_to_user_ptr(req.original_addr), PAGE_SIZE) ||
	    copy_from_user(images + PAGE_SIZE, u64_to_user_ptr(req.current_addr), PAGE_SIZE) ||
	    copy_from_user(images + 2 * PAGE_SIZE,
			   u64_to_user_ptr(req.replacement_addr), PAGE_SIZE))
		goto out;
	ret = -ESTALE;
	if (memcmp(images + offset_in_page(s->gpa), s->data, s->data_len))
		goto out;
	ret = kvm_update_xom_page(vcpu, req.gpa, req.expected_generation, req.request_id,
				  images, images + PAGE_SIZE, images + 2 * PAGE_SIZE);
	if (!ret) {
		state->retry = true;
		state->acknowledged = true;
	}
out:
	kfree(images);
	return ret;
}

long kvm_hbt_ioctl(struct kvm_vcpu *vcpu, unsigned int cmd, void __user *argp)
{
	struct kvm_hbt_state *state = vcpu->arch.hbt;

	if (!vcpu->kvm->arch.hbt_ud_enabled)
		return -EOPNOTSUPP;
	if (!state)
		return -ENOENT;

	switch (cmd) {
	case KVM_HBT_GET_XOM_PAGE:
		return kvm_hbt_get_xom_page(vcpu, state, argp);
	case KVM_HBT_UPDATE_XOM:
		return kvm_hbt_update_xom(vcpu, state, argp);
	case KVM_HBT_TRANSLATE_RW: {
		struct kvm_hbt_translation req;
		struct x86_exception exception = {};
		gpa_t gpa;

		if (!vcpu->kvm->arch.hbt_xom_enabled)
			return -EOPNOTSUPP;
		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;
		if (req.version != KVM_HBT_ABI_VERSION || req.reserved ||
		    req.physical_address || offset_in_page(req.linear_address))
			return -EINVAL;
		if (req.request_id != state->snapshot.request_id ||
		    !kvm_hbt_same_context(vcpu, state))
			return -ESTALE;
		if (state->acknowledged)
			return -EALREADY;
		gpa = kvm_mmu_gva_to_gpa_write(vcpu, req.linear_address, &exception);
		if (gpa == INVALID_GPA)
			return -EFAULT;
		req.physical_address = gpa;
		return copy_to_user(argp, &req, sizeof(req)) ? -EFAULT : 0;
	}
	case KVM_HBT_INSTALL_XOM:
		return kvm_hbt_install_xom(vcpu, state, argp);
	case KVM_HBT_GET_SNAPSHOT: {
		struct kvm_hbt_snapshot_request req;

		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;
		if (req.version != KVM_HBT_ABI_VERSION ||
		    req.size != sizeof(state->snapshot))
			return -EINVAL;
		if (req.request_id != state->snapshot.request_id)
			return -ESTALE;
		if (copy_to_user(u64_to_user_ptr(req.userspace_addr),
				 &state->snapshot, sizeof(state->snapshot)))
			return -EFAULT;
		return 0;
	}
	case KVM_HBT_COMPLETE: {
		struct kvm_hbt_completion reply;

		if (copy_from_user(&reply, argp, sizeof(reply)))
			return -EFAULT;
		if (reply.version != KVM_HBT_ABI_VERSION || reply.reserved ||
		    reply.action > KVM_HBT_COMPLETE_RETRY)
			return -EINVAL;
		if (reply.request_id != state->snapshot.request_id)
			return -ESTALE;
		if (state->acknowledged)
			return -EALREADY;
		if (reply.action == KVM_HBT_COMPLETE_RETRY) {
			if (!vcpu->kvm->arch.hbt_retry_enabled)
				return -EOPNOTSUPP;
			if (!kvm_hbt_same_context(vcpu, state))
				return -ESTALE;
			state->retry = true;
		}
		state->acknowledged = true;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}
