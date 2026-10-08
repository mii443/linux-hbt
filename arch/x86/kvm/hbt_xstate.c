// SPDX-License-Identifier: GPL-2.0
/* Host-independent XSAVE64/XRSTOR64 codec for the HBT virtual CPU. */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "hbt_xstate.h"

static const u32 component_size[8] = { 0, 0, 256, 0, 0, 64, 512, 1024 };

/* AMD MXCSR.MM is bit 17. Only the virtual CPU's mask may enable it. */
#define HBT_MXCSR_KNOWN_MASK 0x0002ffffU

static void *component(struct hbt_xstate *state, unsigned int i)
{
	switch (i) {
	case 2: return state->ymm_hi;
	case 5: return state->opmask;
	case 6: return state->zmm_hi;
	case 7: return state->hi16_zmm;
	default: return NULL;
	}
}

static bool valid_mask(u64 mask)
{
	if (!(mask & HBT_XSTATE_FP) || mask & ~HBT_XSTATE_SUPPORTED)
		return false;
	if ((mask & HBT_XSTATE_YMM) && !(mask & HBT_XSTATE_SSE))
		return false;
	if (mask & HBT_XSTATE_AVX512)
		return (mask & (HBT_XSTATE_AVX512 | HBT_XSTATE_YMM)) ==
			(HBT_XSTATE_AVX512 | HBT_XSTATE_YMM);
	return true;
}

void hbt_xstate_init(struct hbt_xstate *state)
{
	memset(state, 0, sizeof(*state));
	state->xcr0 = HBT_XSTATE_FP;
	put_unaligned_le16(0x37f, state->legacy); /* x87 control word */
	put_unaligned_le32(0x1f80, state->legacy + 24);
}

int hbt_xstate_layout_valid(const struct hbt_xstate_layout *layout)
{
	unsigned int i, j;
	u64 end;

	if (!valid_mask(layout->supported) ||
	    layout->align64 & ~((u32)layout->supported & ~3U) ||
	    (layout->mxcsr_mask & ~HBT_MXCSR_KNOWN_MASK) ||
	    (layout->mxcsr_mask & 0x1f80) != 0x1f80)
		return -EINVAL;
	for (i = 0; i < 8; i++) {
		if (i < 2 || !(layout->supported & (1ULL << i))) {
			if (layout->offset[i])
				return -EINVAL;
			continue;
		}
		end = (u64)layout->offset[i] + component_size[i];
		if (layout->offset[i] < 576 || end > (u32)~0U)
			return -EINVAL;
		for (j = 2; j < i; j++) {
			if ((layout->supported & (1ULL << j)) &&
			    layout->offset[i] < (u64)layout->offset[j] + component_size[j] &&
			    layout->offset[j] < end)
				return -EINVAL;
		}
	}
	return 0;
}

int hbt_xstate_valid(const struct hbt_xstate_layout *layout,
		     const struct hbt_xstate *state)
{
	if (hbt_xstate_layout_valid(layout) || !valid_mask(state->xcr0) ||
	    state->xcr0 & ~layout->supported ||
	    state->xinuse & ~layout->supported ||
	    get_unaligned_le32(state->legacy + 24) & ~layout->mxcsr_mask)
		return -EINVAL;
	return 0;
}

static void offsets(const struct hbt_xstate_layout *layout, u64 mask,
		    bool compacted, u32 offset[8])
{
	u32 next = 576;
	unsigned int i;

	memcpy(offset, layout->offset, sizeof(layout->offset));
	if (!compacted)
		return;
	for (i = 2; i < 8; i++) {
		if (!(mask & (1ULL << i)))
			continue;
		if (layout->align64 & (1U << i))
			next = (next + 63) & ~63U;
		offset[i] = next;
		next += component_size[i];
	}
}

int hbt_xstate_save(const struct hbt_xstate_layout *layout,
		    const struct hbt_xstate *state, enum hbt_xstate_format format,
		    u64 requested, const struct hbt_xstate_io *io)
{
	u64 mask = requested & state->xcr0, written, bv;
	u32 offset[8];
	u8 header[16], mxcsr[8];
	bool compacted = format == HBT_XSAVEC;
	unsigned int i;
	int ret;

	if (hbt_xstate_valid(layout, state) || !io->write ||
	    format < HBT_XSAVE || format > HBT_XSAVEC ||
	    (!compacted && !io->read))
		return -EINVAL;
	if (compacted && !layout->compacted)
		return -EOPNOTSUPP;
	written = format == HBT_XSAVE ? mask : mask & state->xinuse;
	if (compacted && (mask & HBT_XSTATE_SSE) &&
	    get_unaligned_le32(state->legacy + 24) != 0x1f80)
		written |= HBT_XSTATE_SSE;
	if (compacted) {
		bv = written;
	} else {
		ret = io->read(io->opaque, 512, header, 8);
		if (ret)
			return ret;
		bv = (get_unaligned_le64(header) & ~mask) | (state->xinuse & mask);
	}
	/* Do not save reserved legacy bytes or software-owned bytes 464..511. */
	if (written & HBT_XSTATE_FP) {
		ret = io->write(io->opaque, 0, state->legacy, 24);
		if (ret)
			return ret;
		for (i = 0; i < 8; i++) {
			ret = io->write(io->opaque, 32 + 16 * i, state->legacy + 32 + 16 * i, 10);
			if (ret)
				return ret;
		}
	}
	if (written & HBT_XSTATE_SSE) {
		ret = io->write(io->opaque, 160, state->legacy + 160, 256);
		if (ret)
			return ret;
	}
	if (compacted ? !!(written & HBT_XSTATE_SSE) : !!(mask & (HBT_XSTATE_SSE | HBT_XSTATE_YMM))) {
		memcpy(mxcsr, state->legacy + 24, 4);
		put_unaligned_le32(layout->mxcsr_mask, mxcsr + 4);
		ret = io->write(io->opaque, 24, mxcsr, sizeof(mxcsr));
		if (ret)
			return ret;
	}
	offsets(layout, mask, compacted, offset);
	for (i = 2; i < 8; i++) {
		if (!(written & (1ULL << i)))
			continue;
		ret = io->write(io->opaque, offset[i],
				component((struct hbt_xstate *)state, i), component_size[i]);
		if (ret)
			return ret;
	}
	put_unaligned_le64(bv, header);
	put_unaligned_le64(HBT_XSTATE_COMPACT | mask, header + 8);
	return io->write(io->opaque, 512, header, compacted ? 16 : 8);
}

int hbt_xstate_restore(const struct hbt_xstate_layout *layout,
		       const struct hbt_xstate *state, struct hbt_xstate *result,
		       u64 requested, const struct hbt_xstate_io *io)
{
	u64 mask = requested & state->xcr0, bv, comp, load;
	u32 offset[8];
	u8 header[64];
	bool compacted;
	unsigned int i;
	int ret;

	if (state == result || hbt_xstate_valid(layout, state) || !io->read)
		return -EINVAL;
	/* Standard XRSTOR validates bytes 8..23; the remainder is ignored. */
	ret = io->read(io->opaque, 512, header, 24);
	if (ret)
		return ret;
	bv = get_unaligned_le64(header);
	comp = get_unaligned_le64(header + 8);
	compacted = !!(comp & HBT_XSTATE_COMPACT);
	if (compacted) {
		if (!layout->compacted || (comp & ~HBT_XSTATE_COMPACT & ~state->xcr0) ||
		    (bv & ~(comp & ~HBT_XSTATE_COMPACT)))
			return -EINVAL;
		ret = io->read(io->opaque, 536, header + 24, 40);
		if (ret)
			return ret;
	} else if (comp || (bv & ~state->xcr0)) {
		return -EINVAL;
	}
	for (i = 16; i < (compacted ? 64 : 24); i++)
		if (header[i])
			return -EINVAL;
	*result = *state;
	load = mask & bv;
	if (mask & HBT_XSTATE_FP) {
		memset(result->legacy, 0, 24);
		memset(result->legacy + 32, 0, 128);
		put_unaligned_le16(0x37f, result->legacy);
		if (load & HBT_XSTATE_FP) {
			ret = io->read(io->opaque, 0, result->legacy, 24);
			if (ret)
				return ret;
			for (i = 0; i < 8; i++) {
				ret = io->read(io->opaque, 32 + 16 * i, result->legacy + 32 + 16 * i, 10);
				if (ret)
					return ret;
			}
		}
	}
	/* Standard XRSTOR loads MXCSR even for init SSE or a YMM-only request. */
	if ((!compacted && (mask & (HBT_XSTATE_SSE | HBT_XSTATE_YMM))) ||
	    (compacted && (load & HBT_XSTATE_SSE))) {
		ret = io->read(io->opaque, 24, result->legacy + 24, 4);
		if (ret)
			return ret;
		if (get_unaligned_le32(result->legacy + 24) & ~layout->mxcsr_mask)
			return -EINVAL;
	} else if (compacted && (mask & HBT_XSTATE_SSE)) {
		put_unaligned_le32(0x1f80, result->legacy + 24);
	}
	if (mask & HBT_XSTATE_SSE) {
		memset(result->legacy + 160, 0, 256);
		if (load & HBT_XSTATE_SSE) {
			ret = io->read(io->opaque, 160, result->legacy + 160, 256);
			if (ret)
				return ret;
		}
	}
	offsets(layout, comp, compacted, offset);
	for (i = 2; i < 8; i++) {
		if (!(mask & (1ULL << i)))
			continue;
		memset(component(result, i), 0, component_size[i]);
		if (!(load & (1ULL << i)))
			continue;
		ret = io->read(io->opaque, offset[i], component(result, i), component_size[i]);
		if (ret)
			return ret;
	}
	result->xinuse = (state->xinuse & ~mask) | load;
	return 0;
}
