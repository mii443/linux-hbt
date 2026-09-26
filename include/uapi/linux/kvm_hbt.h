/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_KVM_HBT_H
#define _UAPI_LINUX_KVM_HBT_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* Private linux-hbt ABI, not an upstream KVM capability or exit number. */
#define KVM_CAP_HBT_X86_UD	0x48425401
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
struct kvm_hbt_completion {
	__u32 version;
	__u32 action;
	__u64 request_id;
	__u64 reserved;
};

/* Private vCPU ioctls. KVMIO is 0xae; keep this header self-contained. */
#define KVM_HBT_GET_SNAPSHOT \
	_IOW(0xae, 0xe8, struct kvm_hbt_snapshot_request)
#define KVM_HBT_COMPLETE \
	_IOW(0xae, 0xe9, struct kvm_hbt_completion)

#endif /* _UAPI_LINUX_KVM_HBT_H */
