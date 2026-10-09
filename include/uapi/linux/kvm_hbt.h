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
/* Query-only: bounded integer execution in virtual-XSTATE mode, not full ISA. */
#define KVM_CAP_HBT_X86_INTEGER_VECTOR 0x48425407
/* Single-vCPU #UD/RETRY dispatch combined with virtual XSTATE, without XOM. */
#define KVM_CAP_HBT_X86_EPTP_DISPATCH 0x48425408
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
#define KVM_HBT_INTEGER_VECTOR_VERSION 1
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

/*
 * Bounded VMX laboratory interface, NOT a migratable RAM/translation API.
 * CONFIG copies nr_pages of RAM at GPA 0 and up to MAX_OVERLAYS private helper pages.
 * Other pages are shared by the two EPT views. READ copies one complete view.
 * Every hardware exit is returned without guest instruction emulation:
 * run->internal.suberror = raw VMX exit reason; data[0..4] = view,
 * qualification, instruction length, interrupt information, guest RIP.
 * ndata >= 6: data[5] requires recovery for a deferred guest event. A private
 * slice-timer exit (reason 52) with data[5] == 0 can resume the existing views.
 * With LAZY_DATA, ndata >= 7: data[6] is the guest physical address of an
 * EPT violation (zero for other exits). No host physical address is exposed.
 */
#define KVM_HBT_EPTP_VERSION 1
#define KVM_HBT_EPTP_QUERY 0
#define KVM_HBT_EPTP_CONFIG 1
#define KVM_HBT_EPTP_READ 2
#define KVM_HBT_EPTP_DESTROY 3
#define KVM_HBT_EPTP_PENDING_EVENT 0xffffffffU /* suberror: no VM entry */
#define KVM_HBT_EPTP_LIVE_CONFIG 4
#define KVM_HBT_EPTP_READ_MAPS 5
#define KVM_HBT_EPTP_MAP_DATA 6 /* nr_pages is the guest RAM page number */
#define KVM_HBT_EPTP_SUSPEND 7 /* retain an inactive live context */
#define KVM_HBT_EPTP_RESUME 8 /* refresh its private images, enter normal view */
#define KVM_HBT_EPTP_CACHE_CONFIG 9 /* LIVE_CONFIG with nonzero reserved[0] key */
#define KVM_HBT_EPTP_CACHE_RESUME 10 /* resume the suspended reserved[0] key */
#define KVM_HBT_EPTP_CACHE_DROP 11 /* discard an inactive reserved[0] key */
#define KVM_HBT_EPTP_READ_RANGES 12 /* bounded private reads; returns active view */
#define KVM_HBT_EPTP_RESUME_RANGES 13 /* refresh ranges; optional reserved[0] key */
#define KVM_HBT_EPTP_MAX_CONTEXTS 64
#define KVM_HBT_EPTP_MAX_PAGES 512
#define KVM_HBT_EPTP_MAX_OVERLAYS 256
#define KVM_HBT_EPTP_UNDER_HYPERVISOR 1 /* QUERY output; timing is not L0 evidence */
#define KVM_HBT_EPTP_SHARED_OVERLAY 2 /* QUERY: identical image pointers share backing */
#define KVM_HBT_EPTP_LAZY_DATA 4 /* QUERY: MAP_DATA and exit data[6] GPA */
#define KVM_HBT_EPTP_PERSISTENT 8 /* QUERY: SUSPEND/RESUME and larger map budget */
#define KVM_HBT_EPTP_CONTEXT_CACHE 16 /* QUERY: keyed operations; reserved[0] limit */
#define KVM_HBT_EPTP_RANGE_IO 32 /* QUERY: READ_RANGES/RESUME_RANGES */
#define KVM_EXIT_HBT_EPTP 0x48425402

struct kvm_hbt_eptp_overlay {
	__u32 page;
	__u32 reserved;
	__u64 image_addr;
};

/* Range operations use overlays_addr/nr_overlays for this array, with all
 * other arguments zero except an optional RESUME_RANGES context key. Each
 * length is 1..4096, offset+length <= 4096, view is 0/1, flags must be zero.
 * Reads require private backing; resume writes additionally require W in
 * the selected view. Unlisted bytes stay unchanged. All descriptors are
 * validated before copying. A failed refresh leaves the context inactive;
 * successfully copied prefixes may have changed and must be refreshed on retry.
 * The VMM must still validate code/PTEs and supply current canonical state.
 */
struct kvm_hbt_eptp_range {
	__u32 page;
	__u16 offset, length;
	__u64 image_addr;
	__u32 view, flags;
};

/* LIVE_CONFIG bounds user RAM at GPA 0 by nr_pages (at most 1 GiB).
 * Only explicitly listed RAM pages are pinned; all other GPAs are unmapped.
 * Up to MAX_OVERLAYS mappings: permissions bits 2:0 are N R/W/X, bits 10:8 H.
 * A zero image pointer selects pinned RAM; nonzero copies a private page.
 * With SHARED_OVERLAY, repeated nonzero pointers alias the same private copy
 * across maps/views. Each EPT leaf retains its own access permissions.
 * Extra GPAs below 1 GiB are allowed only with private images. READ_MAPS
 * uses an array of kvm_hbt_eptp_overlay as page/destination pairs, nr_pages
 * and image_addr zero, and view 0/1. This remains an opt-in laboratory ABI.
 * MAP_DATA adds one ordinary RAM page to both views as read/write, never
 * executable. nr_pages is its GPA page index inside the original LIVE_CONFIG
 * RAM range; all other request inputs are zero. Explicit overlays cannot be
 * upgraded. Up to 256 additional pages are pinned until DESTROY; writes are
 * conservatively marked dirty in the canonical KVM memory slot.
 * SUSPEND removes the private views from execution without freeing them.
 * RESUME recopies writable private images from the original LIVE_CONFIG user pointers;
 * Read/execute-only images remain immutable until the next LIVE_CONFIG;
 * the VMM must first validate code/page tables and refresh architectural state.
 * Both take zero arguments beyond version/operation. A failed refresh stays
 * suspended. Successful LIVE_CONFIG replaces any suspended context; DESTROY
 * frees both active and suspended contexts. No cache contents are VM state.
 * CONTEXT_CACHE additionally supports nonzero caller keys in reserved[0] for
 * CACHE_CONFIG/RESUME/DROP. Other reserved fields remain zero. CACHE_CONFIG
 * replaces the same inactive key only after successful construction. SUSPEND
 * parks keyed contexts in a bounded LRU; CACHE_RESUME may return ENOENT after
 * eviction. Failed refresh leaves the selected context inactive and retryable.
 * CACHE_DROP rejects active keys; DESTROY releases all keyed and legacy views.
 * QUERY reports MAX_CONTEXTS in reserved[0] (input must still be zero).
 */
struct kvm_hbt_eptp_live_map {
	__u32 page;
	__u32 permissions;
	__u64 normal_addr;
	__u64 helper_addr;
};
#define KVM_HBT_EPTP_LIVE_MAX_PAGES (1U << 18)

struct kvm_hbt_eptp_probe {
	__u32 version;
	__u32 operation;
	__u32 nr_pages;
	__u32 view;
	__u64 image_addr;
	__u64 overlays_addr;
	__u32 nr_overlays;
	__u32 flags;
	__u64 reserved[3];
};

#define KVM_HBT_EPTP_PROBE \
	_IOWR(0xae, 0xf0, struct kvm_hbt_eptp_probe)

#endif /* _UAPI_LINUX_KVM_HBT_H */
