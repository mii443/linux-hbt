/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_VMX_XOM_H
#define __KVM_VMX_XOM_H

#include <linux/errno.h>
#include <asm/insn.h>

#define KVM_XOM_CODE_CAVE_MIN_LEN 32

static inline bool vmx_xom_insn_is_nop(const struct insn *insn)
{
	unsigned int i;

	/* Exclude PAUSE, LOCK, register exchanges and emulation prefixes. */
	if (insn->vex_prefix.nbytes || insn->rex_prefix.nbytes ||
	    insn->emulate_prefix_size)
		return false;

	for (i = 0; i < insn->prefixes.nbytes; i++) {
		switch (insn->kaddr[i]) {
		case 0x26: case 0x2e: case 0x36: case 0x3e:
		case 0x64: case 0x65: case 0x66: case 0x67:
			break;
		default:
			return false;
		}
	}

	if (insn->opcode.nbytes == 1 && insn->opcode.bytes[0] == 0x90)
		return true;

	return insn->opcode.nbytes == 2 &&
	       insn->opcode.bytes[0] == 0x0f &&
	       insn->opcode.bytes[1] == 0x1f &&
	       insn->modrm.nbytes && !X86_MODRM_REG(insn->modrm.value);
}

/*
 * Decode forward from a known instruction boundary.  A page boundary need
 * not be an instruction boundary, so never restart there or resynchronize
 * after a decoding error.  The result is only a heuristic candidate: even
 * genuine NOPs can be reachable code or data referenced by the guest.
 */
static inline int vmx_xom_find_nop_run(const u8 *page, unsigned int size,
				     unsigned int start, enum insn_mode mode,
				     unsigned int min_len,
				     unsigned int *offset, unsigned int *length)
{
	unsigned int pos = start, run_start = 0, run = 0;
	struct insn insn;
	int ret;

	*offset = 0;
	*length = 0;
	if (!min_len || min_len > size || start > size ||
	    (mode != INSN_MODE_32 && mode != INSN_MODE_64))
		return -EINVAL;

	while (pos < size) {
		ret = insn_decode(&insn, page + pos, size - pos, mode);
		if (ret)
			return ret;
		if (!insn.length || insn.length > size - pos)
			return -EINVAL;

		if (vmx_xom_insn_is_nop(&insn)) {
			if (!run)
				run_start = pos;
			run += insn.length;
			if (run >= min_len) {
				*offset = run_start;
				*length = run;
				return 0;
			}
		} else {
			run = 0;
		}
		pos += insn.length;
	}

	return -ENOENT;
}

#endif /* __KVM_VMX_XOM_H */
