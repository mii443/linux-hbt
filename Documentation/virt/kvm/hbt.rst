.. SPDX-License-Identifier: GPL-2.0

Experimental HBT userspace #UD analysis
=======================================

This linux-hbt extension is a private #UD interface, not an upstream KVM ABI.
The definitions in ``include/uapi/linux/kvm_hbt.h`` use private capability and
exit number ``0x48425401``. No instruction decoding, candidate selection or
patch installation is performed by this interface.

KVM_CAP_HBT_X86_UD
------------------

:Architectures: x86 (VMX only)
:Type: VM capability
:Parameters: args[0] = KVM_HBT_ABI_VERSION (1); args[1..3] and flags = 0

KVM_CHECK_EXTENSION returns 1 when the backend supports this version, or 0
otherwise. A VM-specific check also requires KVM_X86_DEFAULT_VM. Enabling
after any vCPU has been created fails with EBUSY. Unsupported version,
parameters, backend or VM type fail with EINVAL. Once enabled, the capability
cannot be disabled on an existing VM.

For eligible #UD exceptions, KVM captures the current 32-bit or 64-bit
instruction stream and exits with KVM_EXIT_HBT_X86_UD. Capture starts at the
linear instruction address and ends at that 4 KiB page's end. The maximum
length is KVM_HBT_MAX_BYTES. The GPA is translated with instruction-fetch
access before reading the bytes. Other vCPUs and guest-memory writers may
change memory during or after capture; this is not an atomic snapshot or a
reservation for code placement.

Nested/protected guests, 16-bit/vm86 execution, vectoring events and capture
failures continue through normal #UD handling. The capability takes precedence
over the separately enabled legacy VMX XOM skip experiment.

KVM_CAP_HBT_X86_RETRY
--------------------

:Architectures: x86 (VMX only)
:Type: VM capability, private number 0x48425402
:Parameters: args[0] = KVM_HBT_ABI_VERSION (1); args[1..3] and flags = 0

KVM_CHECK_EXTENSION returns 1 when supported. Enable KVM_CAP_HBT_X86_UD first,
then enable this capability before creating any vCPU. Otherwise enabling fails
with EINVAL or, when vCPUs already exist, EBUSY. Enabling it permanently limits
the VM to one vCPU; creating a second vCPU fails with EINVAL.

This permits explicit RETRY completions after a trusted VMM has installed and
validated replacement guest code. It does not install code, reserve memory,
validate replacement semantics or exclude userspace/DMA writers. The VMM owns
those operations and must roll back its writes if acknowledgment fails. The
default observation capability alone never permits RETRY.

KVM_EXIT_HBT_X86_UD
-------------------

The output-only ``kvm_run.hbt`` record contains version, a zero reserved field
and a nonzero request_id. IDs are per-vCPU and are not restarted by RESET/INIT.
The vCPU retains at most one pending request. The exit union's size is unchanged;
the byte buffer is accessed by a separate ioctl. KVM never reads completion
data from the shared kvm_run record.

KVM_HBT_GET_SNAPSHOT
--------------------

:Type: vCPU ioctl
:Parameters: struct kvm_hbt_snapshot_request

Set version to 1, size to sizeof(struct kvm_hbt_snapshot), request_id to the
exit's ID, and userspace_addr to a writable buffer of that size. The captured
snapshot contains version, ID, architectural RIP, linear RIP, GPA, CR3,
execution mode (32 or 64), data_len and data. Reserved fields and bytes beyond
data_len are zero. Repeated reads of a pending request return the same copy.
Snapshot data is not modified by an acknowledgment.

KVM_HBT_COMPLETE
----------------

:Type: vCPU ioctl
:Parameters: struct kvm_hbt_completion

Set version to 1, request_id to the exit's ID, action to
KVM_HBT_COMPLETE_FALLBACK (0), and reserved to zero. This acknowledges the
request. The next KVM_RUN frees the snapshot
and directly calls the normal #UD handler, avoiding interception of the same
fault for a second time. KVM does not change guest bytes or RIP for completion.

With KVM_CAP_HBT_X86_RETRY enabled, action KVM_HBT_COMPLETE_RETRY (1) instead
acknowledges a userspace-installed patch. The ioctl requires the saved RIP,
linear address, mode, control-register context and exception state to still
match. The next KVM_RUN consumes the request without invoking the #UD handler
or advancing RIP, allowing the patched entry instruction to execute. Merely
enabling the capability does not request a retry; missing replies still fall
back normally. Snapshot bytes remain the original captured bytes after patching.

If userspace omits the acknowledgment, the next KVM_RUN performs the same
fallback. Normal #UD emulation may itself exit to userspace through existing
KVM interfaces. If userspace replaces RIP, its linear address, mode, control
register context or pending exception state before resuming, the stale fault
context is discarded. RESET/INIT and vCPU destruction also discard pending
state. A VMM must resolve pending requests before saving/migrating a vCPU;
this experimental interface has no migration state format.

Errors
------

Both ioctls return EOPNOTSUPP when the VM has not enabled the capability, and
ENOENT when no request is pending. A mismatched ID returns ESTALE. Invalid
version, buffer size, action or reserved field returns EINVAL. User-copy
failures return EFAULT. Repeating a successful acknowledgment before KVM_RUN
returns EALREADY. An invalid request does not consume or change pending state.
RETRY without its separate capability returns EOPNOTSUPP; RETRY with a replaced
fault context returns ESTALE. Undefined action values return EINVAL.
