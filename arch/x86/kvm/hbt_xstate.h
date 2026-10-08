/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_HBT_XSTATE_H
#define __KVM_X86_HBT_XSTATE_H

#include <linux/types.h>

#define HBT_XSTATE_FP       (1ULL << 0)
#define HBT_XSTATE_SSE      (1ULL << 1)
#define HBT_XSTATE_YMM      (1ULL << 2)
#define HBT_XSTATE_OPMASK   (1ULL << 5)
#define HBT_XSTATE_ZMM_HI   (1ULL << 6)
#define HBT_XSTATE_HI16     (1ULL << 7)
#define HBT_XSTATE_AVX512   (HBT_XSTATE_OPMASK | HBT_XSTATE_ZMM_HI | HBT_XSTATE_HI16)
#define HBT_XSTATE_SUPPORTED (7ULL | HBT_XSTATE_AVX512)
#define HBT_XSTATE_COMPACT  (1ULL << 63)

/* Normalized state, NOT a hardware XSAVE area or a userspace ABI. */
struct hbt_xstate {
	u64 xcr0;
	u64 xinuse;
	u8 legacy[512]; /* REX.W=1 legacy representation; no XSAVE header. */
	u8 ymm_hi[16][16];
	u8 opmask[8][8];
	u8 zmm_hi[16][32];
	u8 hi16_zmm[16][64];
};

/* Supplied from the virtual CPU's CPUID. Never derive this from the host. */
struct hbt_xstate_layout {
	u64 supported;
	u32 offset[8]; /* Standard offsets for components 2, 5, 6, 7. */
	u32 align64;   /* CPUID.0D.i:ECX[1], indexed by component. */
	u32 mxcsr_mask; /* Virtual CPU policy, including optional AMD MM (bit 17). */
	bool compacted;
};

/* Offsets are relative to the guest's XSAVE area, not host pointers.
 * A callback performs one complete access or returns a negative error.
 * Writes completed before an error remain visible. No whole-area prefetch.
 */
struct hbt_xstate_io {
	int (*read)(void *opaque, u32 offset, void *data, u32 size);
	int (*write)(void *opaque, u32 offset, const void *data, u32 size);
	void *opaque;
};

enum hbt_xstate_format {
	HBT_XSAVE,
	HBT_XSAVEOPT,
	HBT_XSAVEC,
};

void hbt_xstate_init(struct hbt_xstate *state);
int hbt_xstate_layout_valid(const struct hbt_xstate_layout *layout);
int hbt_xstate_valid(const struct hbt_xstate_layout *layout,
		     const struct hbt_xstate *state);
int hbt_xstate_save(const struct hbt_xstate_layout *layout,
		    const struct hbt_xstate *state, enum hbt_xstate_format format,
		    u64 requested, const struct hbt_xstate_io *io);
/* Prepare a new state; the caller commits only after success. On failure,
 * result is unspecified, state is untouched. result must not alias state.
 * This is a codec, not an instruction dispatcher: mode/CPUID/CR0/CR4/CPL,
 * address alignment, exception priority and FPU synchronization belong to
 * the execution adapter. XSAVES/XRSTORS and nonzero XSS are not enabled here.
 */
int hbt_xstate_restore(const struct hbt_xstate_layout *layout,
		       const struct hbt_xstate *state, struct hbt_xstate *result,
		       u64 requested, const struct hbt_xstate_io *io);

#endif
