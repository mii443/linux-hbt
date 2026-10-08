/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_KVM_HBT_H
#define _UAPI_LINUX_KVM_HBT_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* Private linux-hbt ABI, not an upstream KVM capability or exit number. */
#define KVM_CAP_HBT_X86_UD	0x48425401
#define KVM_CAP_HBT_X86_RETRY	0x48425402
#define KVM_CAP_HBT_X86_XOM	0x48425403
#define KVM_CAP_HBT_X86_XOM_UPDATE 0x48425404 /* Query only; enabled with XOM. */
#define KVM_CAP_HBT_X86_XSTATE_STORAGE 0x48425405
#define KVM_CAP_HBT_X86_VIRTUAL_XSTATE 0x48425406
#define KVM_EXIT_HBT_X86_UD	0x48425401
#define KVM_HBT_ABI_VERSION	1
#define KVM_HBT_MAX_BYTES	4096

/* Output only. No fields in kvm_run are consumed as a completion. */
struct kvm_hbt_exit {
	__u32 version;
	__u32 reserved;
	__u64 request_id;
};

/* Bytes start at linear_rip and stop at the end of its 4 KiB page. */
struct kvm_hbt_snapshot {
	__u32 version;
	__u32 reserved;
	__u64 request_id;
	__u64 rip;
	__u64 linear_rip;
	__u64 gpa;
	__u64 cr3;
	__u32 mode;
	__u32 data_len;
	__u64 reserved2;
	__u8 data[KVM_HBT_MAX_BYTES];
};

struct kvm_hbt_snapshot_request {
	__u32 version;
	__u32 size;
	__u64 request_id;
	__u64 userspace_addr;
};

#define KVM_HBT_COMPLETE_FALLBACK	0
#define KVM_HBT_COMPLETE_RETRY	1
struct kvm_hbt_completion {
	__u32 version;
	__u32 action;
	__u64 request_id;
	__u64 reserved;
};

/*
 * Install one aligned 4 KiB page and acknowledge RETRY in one operation.
 * Both user pointers address KVM_HBT_MAX_BYTES bytes. No guest writes may
 * precede this ioctl. On error, guest bytes and pending completion are intact.
 */
struct kvm_hbt_xom_install {
	__u32 version;
	__u32 reserved;
	__u64 request_id;
	__u64 gpa;
	__u64 original_addr;
	__u64 replacement_addr;
};

/*
 * Translate a page for data reads/writes at the faulting context's CPL.
 * Valid only while an XOM-enabled #UD request is pending, before completion.
 */
struct kvm_hbt_translation {
	__u32 version;
	__u32 reserved;
	__u64 request_id;
	__u64 linear_address;
	__u64 physical_address; /* Output; input must be zero. */
};

/*
 * Read the original/current page and generation while a #UD is pending.
 * generation is output (zero on input); zero means no installed translation.
 */
struct kvm_hbt_xom_page {
	__u32 version;
	__u32 reserved;
	__u64 request_id;
	__u64 gpa;
	__u64 original_addr;
	__u64 current_addr;
	__u64 generation;
};

/*
 * Compare-and-replace an entire page; preserve its first original. Success
 * sets generation=request_id and acknowledges RETRY. No output copy can fail
 * after publication. A write/discard makes every previous generation stale.
 */
struct kvm_hbt_xom_update {
	__u32 version;
	__u32 reserved;
	__u64 request_id;
	__u64 gpa;
	__u64 expected_generation;
	__u64 original_addr;
	__u64 current_addr;
	__u64 replacement_addr;
};

/* Software-only AVX-512 components. This is not a hardware XSAVE area.
 * Storage does not advertise or emulate AVX-512 guest instructions.
 * xfeatures must equal KVM_HBT_XSTATE_FEATURES; xinuse is a subset.
 * SET canonicalizes components absent from xinuse to zero.
 */
#define KVM_HBT_XSTATE_VERSION 1
#define KVM_HBT_VIRTUAL_XSTATE_VERSION 1
#define KVM_HBT_XSTATE_FEATURES ((__u64)0xe0)
struct kvm_hbt_xstate {
	__u32 version;
	__u32 size;
	__u64 xfeatures;
	__u64 xinuse;
	__u64 reserved[5];
	__u8 opmask[8][8];
	__u8 zmm_hi[16][32];
	__u8 hi16_zmm[16][64];
};

/* Private vCPU ioctls. KVMIO is 0xae; keep this header self-contained. */
#define KVM_HBT_GET_SNAPSHOT \
	_IOW(0xae, 0xe8, struct kvm_hbt_snapshot_request)
#define KVM_HBT_COMPLETE \
	_IOW(0xae, 0xe9, struct kvm_hbt_completion)
#define KVM_HBT_INSTALL_XOM \
	_IOW(0xae, 0xea, struct kvm_hbt_xom_install)
#define KVM_HBT_TRANSLATE_RW \
	_IOWR(0xae, 0xeb, struct kvm_hbt_translation)
#define KVM_HBT_GET_XOM_PAGE \
	_IOWR(0xae, 0xec, struct kvm_hbt_xom_page)
#define KVM_HBT_UPDATE_XOM \
	_IOW(0xae, 0xed, struct kvm_hbt_xom_update)
#define KVM_HBT_GET_XSTATE \
	_IOR(0xae, 0xee, struct kvm_hbt_xstate)
#define KVM_HBT_SET_XSTATE \
	_IOW(0xae, 0xef, struct kvm_hbt_xstate)

#endif /* _UAPI_LINUX_KVM_HBT_H */
