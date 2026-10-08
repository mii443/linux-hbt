HBT virtual XSTATE implementation
================================

The implementation has a host-independent XSAVE64/XRSTOR64 codec in
``arch/x86/kvm/hbt_xstate.[ch]``, software AVX-512 register storage, and an
opt-in VMX control path for virtual CPUID/XCR0. The codec is not yet called
by the guest instruction emulator. These pieces do not yet make an AVX-512
VM executable or migratable to an AVX2 host.

The normalized state holds x87/SSE, YMM_Hi128, opmask, ZMM_Hi256 and Hi16_ZMM.
The caller supplies the virtual CPUID.0D layout and MXCSR mask. Standard
offsets may differ from the host; compacted offsets are calculated from
XCOMP_BV and the supplied component alignment flags. Only user components
0, 1, 2, 5, 6 and 7 are supported. Other features, supervisor state, and
non-REX.W legacy representations require subsequent implementation.

The MXCSR mask can include AMD's misaligned-exception mask (MM, bit 17);
bit 16 and bits 31:18 remain reserved. MM is accepted in state only when
the virtual CPU's mask enables it. An Intel profile therefore still rejects
MM. This codec permission does not imply that a host CPU can load MM or
execute the corresponding SSE behavior: the future execution adapter must
also honor the virtual CPU profile. See AMD APM Volume 1, section 4.2.2.

Save supports XSAVE, XSAVEOPT without the optional modified-state optimization,
and XSAVEC. It preserves unrequested standard-header bits and software-owned
legacy bytes. XSAVEOPT honors the init optimization; XSAVEC also handles the
noninitial-MXCSR exception. Restore handles standard and compacted areas,
initializes selected absent components, preserves unselected components, and
validates reserved header fields and MXCSR. MXCSR has distinct standard and
compacted restore rules, including a standard YMM-only request.

Memory callbacks access individual component ranges relative to an XSAVE area.
They must implement the guest's permissions, page faults and dirty accounting.
No entire-area read or write is used. Completed writes remain visible if a
later callback fails. Restore prepares a separate candidate; on failure the
candidate must be discarded. This is not an architectural instruction fault
adapter: the adapter must still implement mode, feature, CR0/CR4/CPL, address
and exception-priority checks and the specified partial-completion behavior.
It must synchronize the live guest FPU before calling the codec, and commit
the candidate to the native subset and software state after success. A guest
buffer must never be passed directly to host XRSTOR.

Validation in the parent repository::

  python3 kernel-work/tests/test-hbt-xstate.py

The harness compiles the actual codec with ASan/UBSan, exercises all 64 request
masks against all 64 in-use masks in three save formats, checks access failures,
virtual-layout holes, malformed headers, reserved bytes and MXCSR behavior,
and compares against native XSAVE64 for all 16 XMM/YMM registers when available.
An additional differential case loads all 32 ZMM and eight 64-bit opmask
registers on AVX-512F/BW hosts (BW is needed for KMOVQ). It explicitly skips
on other hosts; native comparisons have also passed on a Ryzen 9 7950X3D
under WSL2 (results are recorded in the parent repository).

Virtual CPUID and XCR0 control path
----------------------------------

``KVM_CAP_HBT_X86_VIRTUAL_XSTATE`` enables an experimental VMX mode before
vCPU creation. It is separate from, and incompatible with, UD/RETRY/XOM
translation: existing guest AVX2 helpers cannot execute with actual
GUEST_CR4.OSXSAVE forced to zero. Software XSTATE storage is enabled as well.
Only an ordinary VM on a VMX host with native x87/SSE/AVX2 is accepted.

The VMM supplies CPUID via KVM_SET_CPUID2. The host's KVM_GET_SUPPORTED_CPUID
is unchanged; it does not suddenly promise host AVX-512 support. The opt-in
profile must expose XSAVE, AVX, AVX2 and AVX512F, and CPUID.0D.0:EAX=0xe7.
It must include indexed subleaves 0, 1, 2, 5, 6 and 7. Component sizes are
256, 64, 512 and 1024 bytes; offsets come from this virtual profile, not the
host. Validation rejects overlapping/overflowing ranges, wrong sizes,
duplicate/missing subleaves, non-user components and inconsistent total size.
Subleaf 1 is all zero in v1: XSAVEOPT, XSAVEC, XGETBV(1), XSAVES and XFD
are not exposed. Nested VMX/SVM and SGX are rejected. Other CPUID features
remain the VMM's responsibility and must fit the native KVM CPU model.

``vcpu->arch.xcr0`` is guest-visible. It accepts 1, 3, 7 and 0xe7, initially
1, with normal architectural dependency/reserved-bit checks. Hardware loads
use only its x87/SSE/YMM subset. KVM_GET/SET_XCRS transfer the virtual value;
KVM_GET/SET_XSAVE retain the host ABI layout and native subset. Software
opmask/ZMM state is transferred separately with KVM_HBT_GET/SET_XSTATE.
Writing unsupported bits into the native XSAVE image is rejected.

The CR4 read shadow retains the guest OSXSAVE bit and CPUID.1:OSXSAVE follows
that bit. CPUID.0D.0:EBX is recomputed from the virtual XCR0 and virtual
component offsets; ECX remains the full supported size. Intel-style layouts
(2688 bytes) and the tested AMD-style layout (2432 bytes) therefore work on
the same AVX2-only host. Standard KVM restrictions on changing CPUID after
KVM_RUN still apply.

VMX intercepts #UD with actual GUEST_CR4.OSXSAVE=0. The existing instruction
decoder is allowed to emulate XGETBV(0) and XSETBV(0) in this mode; ordinary
#UD handling is not broadened to arbitrary instructions. XGETBV(1) and
invalid indices raise #GP(0); invalid XCR0 writes preserve the previous
value. XSETBV at CPL3 raises #GP(0), except that virtual OSXSAVE=0 raises
#UD first. Illegal LOCK prefixes raise #UD. Other AVX/XSAVE-family
instructions still raise #UD, with no native execution or memory writes.
This is a control-path test mode, not a usable full AVX-512 CPU model.

``kernel-work/tests/hbt-cpuid-live.c`` in the parent repository executes on
the actual KVM backend inside the disposable nested test VM. For each of
the Intel/AMD layouts it tests all 256 low-byte XCR0 values, malformed
profiles and unchanged state on rejection, CPL0/3, 32/64-bit execution,
CR4 read/write shadowing, dynamic CPUID, per-vCPU isolation, native XSAVE
boundaries, and continued trapping of vector/XSAVE instructions.

Remaining integration work
--------------------------

Persistent per-vCPU software storage and a versioned VMM transfer interface
are implemented by ``KVM_CAP_HBT_X86_XSTATE_STORAGE`` (see ``hbt.rst``). The
storage API and the codec are separately tested building blocks; an execution
adapter connecting them to the native FPU is still required.

* Guest XSAVE-family dispatch, guest-memory fault delivery, XSAVES/XRSTORS, native
  FPU synchronization and correct mixed AVX/AVX-512 state changes.
* QEMU import/export, guest context-switch and signal tests, then migration.

Specification: Intel SDM Volume 2D, XSAVE/XSAVEOPT/XSAVEC/XRSTOR, and Volume 1,
Chapter 13 (XSAVE-managed state). The codec's transaction contract must not
be mistaken for complete instruction emulation.
