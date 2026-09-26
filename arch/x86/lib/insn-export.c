// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <asm/insn.h>

/* insn.c is also included by the boot decompressor; keep exports separate. */
EXPORT_SYMBOL_GPL(insn_decode);
