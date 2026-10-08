// SPDX-License-Identifier: GPL-2.0
/* Join the live guest FPU and software AVX-512 state at an XSAVE boundary. */
#include <linux/kvm_host.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <asm/fpu/xstate.h>

#include "cpuid.h"
#include "fpu.h"
#include "hbt.h"
#include "x86.h"

/* The host's architectural legacy/YMM layout, never the virtual CPUID layout.
 * Only the first three components are passed to hardware. All memory here is
 * kernel-owned, initialized and validated; guest memory is accessed via io.
 */
struct hbt_native_xstate {
	u8 legacy[512];
	u64 bv;
	u64 reserved[7];
	u8 ymm[256];
} __aligned(64);

struct hbt_xstate_work {
	struct hbt_native_xstate native;
	struct hbt_xstate before, after;
};

static int native_xstate(struct hbt_native_xstate *native, bool restore,
			 bool rex_w, u32 mask)
{
	int fault;

#define HBT_NATIVE_INSN(insn) ({ \
	int _fault = 0; \
	asm volatile("1: " insn " %[area]\n2:\n" \
		     _ASM_EXTABLE_TYPE_REG(1b, 2b, EX_TYPE_ONE_REG, %[_fault]) \
		     : [area] "+m" (*native), [_fault] "+r" (_fault) \
		     : "a" (mask), "d" (0) : "memory"); \
	_fault; \
})

	kvm_fpu_get();
#ifdef CONFIG_X86_64
	if (rex_w) {
		if (restore)
			fault = HBT_NATIVE_INSN("xrstor64");
		else
			fault = HBT_NATIVE_INSN("xsave64");
	} else
#endif
	{
		if (restore)
			fault = HBT_NATIVE_INSN("xrstor");
		else
			fault = HBT_NATIVE_INSN("xsave");
	}
	kvm_fpu_put();
#undef HBT_NATIVE_INSN
	return fault ? -EIO : 0;
}

static void read_native(struct hbt_xstate *state, const struct hbt_native_xstate *native)
{
	hbt_xstate_init(state);
	state->xinuse = native->bv & 7;
	if (state->xinuse & HBT_XSTATE_FP) {
		memcpy(state->legacy, native->legacy, 24);
		memcpy(state->legacy + 32, native->legacy + 32, 128);
	}
	/* MXCSR is live even with XINUSE.SSE clear. */
	memcpy(state->legacy + 24, native->legacy + 24, 8);
	if (state->xinuse & HBT_XSTATE_SSE)
		memcpy(state->legacy + 160, native->legacy + 160, 256);
	if (state->xinuse & HBT_XSTATE_YMM)
		memcpy(state->ymm_hi, native->ymm, sizeof(state->ymm_hi));
}

int kvm_hbt_emulate_xstate(struct kvm_vcpu *vcpu, enum hbt_xstate_format op,
			 u64 requested, bool mode64, bool rex_w,
			 const struct hbt_xstate_io *io)
{
	struct hbt_xstate_layout layout = {
		.supported = HBT_XSTATE_SUPPORTED,
		.legacy_mode = !mode64,
	};
	struct kvm_hbt_xstate *soft = vcpu->arch.hbt_xstate, *replacement = NULL;
	struct kvm_cpuid_entry2 *entry;
	struct hbt_xstate_work *work;
	void *allocation;
	unsigned int i;
	u32 native_mask;
	int ret;

	if (!vcpu->kvm->arch.hbt_virtual_xstate_enabled ||
	    !vcpu->arch.guest_fpu.fpstate->in_use || is_guest_mode(vcpu))
		return -EOPNOTSUPP;
	entry = kvm_find_cpuid_entry_index(vcpu, 0xd, 1);
	if (!entry)
		return -EOPNOTSUPP;
	layout.compacted = cpuid_entry_has(entry, X86_FEATURE_XSAVEC);
	for (i = 2; i < 8; i++) {
		if (!(layout.supported & BIT_ULL(i)))
			continue;
		entry = kvm_find_cpuid_entry_index(vcpu, 0xd, i);
		if (!entry)
			return -EOPNOTSUPP;
		layout.offset[i] = entry->ebx;
		if (entry->ecx & 2)
			layout.align64 |= BIT(i);
	}
	allocation = kzalloc(sizeof(*work) + 63, GFP_KERNEL_ACCOUNT);
	if (!allocation)
		return -ENOMEM;
	work = PTR_ALIGN(allocation, 64);
	if (op == HBT_XRSTOR) {
		replacement = kzalloc_obj(*replacement, GFP_KERNEL_ACCOUNT);
		if (!replacement) {
			ret = -ENOMEM;
			goto out;
		}
	}
	/* Snapshot all native components, including registers unavailable in the
	 * guest's current execution mode. No guest-memory access holds fpregs_lock.
	 */
	ret = native_xstate(&work->native, false, rex_w, 7);
	if (ret)
		goto out;
	read_native(&work->before, &work->native);
	work->before.xcr0 = vcpu->arch.xcr0;
	layout.mxcsr_mask = get_unaligned_le32(work->native.legacy + 28) & 0xffff;
	if (soft) {
		work->before.xinuse |= soft->xinuse;
		memcpy(work->before.opmask, soft->opmask, sizeof(soft->opmask));
		memcpy(work->before.zmm_hi, soft->zmm_hi, sizeof(soft->zmm_hi));
		memcpy(work->before.hi16_zmm, soft->hi16_zmm, sizeof(soft->hi16_zmm));
	}
	if (op != HBT_XRSTOR) {
		ret = hbt_xstate_save(&layout, &work->before, op, requested, io);
		goto out;
	}
	ret = hbt_xstate_restore(&layout, &work->before, &work->after, requested, io);
	if (ret)
		goto out;

	/* No fallible allocation or guest-memory access after this point. Restore
	 * only requested native components. A legacy-mode init must reload saved
	 * upper registers too; the codec retains their conservative XINUSE bits.
	 */
	memcpy(work->native.legacy, work->after.legacy, 512);
	memcpy(work->native.ymm, work->after.ymm_hi, 256);
	work->native.bv = work->after.xinuse & 7;
	native_mask = requested & vcpu->arch.xcr0 & 7;
	/* Linux's user ABI exports MXCSR only if SSE or YMM is in use. A
	 * standard XRSTOR can load noninitial MXCSR while initializing both.
	 * Conservatively mark a requested component in use to retain MXCSR
	 * through KVM_GET_XSAVE and a subsequent migration/import. Do not alter
	 * unrequested components or depend on the host using compacted saves.
	 */
	if ((native_mask & 6) && get_unaligned_le32(work->native.legacy + 24) != 0x1f80)
		work->native.bv |= native_mask & 2 ? 2 : 4;
	ret = native_xstate(&work->native, true, rex_w, native_mask);
	if (ret)
		goto out;
	replacement->version = KVM_HBT_XSTATE_VERSION;
	replacement->size = sizeof(*replacement);
	replacement->xfeatures = KVM_HBT_XSTATE_FEATURES;
	replacement->xinuse = work->after.xinuse & HBT_XSTATE_AVX512;
	memcpy(replacement->opmask, work->after.opmask, sizeof(replacement->opmask));
	memcpy(replacement->zmm_hi, work->after.zmm_hi, sizeof(replacement->zmm_hi));
	memcpy(replacement->hi16_zmm, work->after.hi16_zmm, sizeof(replacement->hi16_zmm));
	vcpu->arch.hbt_xstate = replacement;
	replacement = NULL;
	kfree(soft);
out:
	kfree(replacement);
	kfree(allocation);
	return ret;
}
