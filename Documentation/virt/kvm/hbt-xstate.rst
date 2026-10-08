HBT virtual XSTATE implementation
================================

The implementation has a host-independent XSAVE/XRSTOR codec in
``arch/x86/kvm/hbt_xstate.[ch]``, software AVX-512 register storage, and an
opt-in VMX path for virtual CPUID/XCR0 and guest XSAVE-family emulation.
``hbt_xstate_emulate.c`` connects the live native FPU to the codec and software
state. Vector execution and QEMU integration are still needed to make an
AVX-512 VM executable or migratable to an AVX2 host.

The normalized state holds x87/SSE, YMM_Hi128, opmask, ZMM_Hi256 and Hi16_ZMM.
The caller supplies the virtual CPUID.0D layout and MXCSR mask. Standard
offsets may differ from the host; compacted offsets are calculated from
XCOMP_BV and the supplied component alignment flags. Only user components
0, 1, 2, 5, 6 and 7 are supported. The adapter selects the native x87 pointer
representation using the instruction's REX.W bit. Outside 64-bit mode it
saves/restores only XMM/YMM/ZMM0..7 and all opmask registers. XMM/YMM/ZMM8..15
and ZMM16..31 are preserved, with conservative in-use tracking so later
64-bit saves do not lose them. Other features and supervisor state are excluded.

The MXCSR mask can include AMD's misaligned-exception mask (MM, bit 17);
bit 16 and bits 31:18 remain reserved. MM is accepted in state only when
the virtual CPU's mask enables it. An Intel profile therefore still rejects
MM. The current execution adapter restricts MXCSR to the host's mask intersected
with 0xffff, so AMD MM cannot be loaded on the Intel execution backend. AMD-style
component offsets do not imply AMD floating-point semantics. Other native
features, including FPU_CS_DS deprecation, must match the host's CPU contract.
See AMD APM Volume 1, section 4.2.2.

Save supports XSAVE, XSAVEOPT without the optional modified-state optimization,
and XSAVEC. It preserves unrequested standard-header bits and software-owned
legacy bytes. XSAVEOPT honors the init optimization; XSAVEC also handles the
noninitial-MXCSR exception. Restore handles standard and compacted areas,
initializes selected absent components, preserves unselected components, and
validates reserved header fields and MXCSR. MXCSR has distinct standard and
compacted restore rules, including a standard YMM-only request.

Memory callbacks access individual component ranges relative to an XSAVE area.
The emulator checks segment limits, canonical addresses and 64-byte alignment,
then uses ordinary KVM RAM translation with guest permissions, page faults and
dirty accounting. A standard save's read/modify/write header access checks
write permission and reports a write #PF even when its initial read faults.
No entire-area read or write is used. Completed save writes remain visible
on a later fault; restore discards its candidate on any guest-memory or
validation fault, leaving architectural state and the faulting RIP unchanged.
This is the adapter's chosen fault behavior, not a promise to reproduce native
processors' partial restore side effects. MMIO XSAVE areas are unsupported;
standard-memory access failures without a guest #PF use normal emulation failure.

The adapter snapshots native x87/SSE/YMM under ``kvm_fpu_get/put`` into an
aligned kernel buffer. It releases the FPU lock before guest-memory accesses
and never passes a guest address, feature bitmap or unvalidated MXCSR to host
XRSTOR. Native hardware sees only components 0..2 in its own 832-byte layout;
the codec maps the remaining components using virtual CPUID offsets. After
successful restore it commits the validated native subset and preallocated
software state under the vCPU lock. Native XINUSE may conservatively retain
SSE or YMM in use when MXCSR is noninitial; otherwise Linux's KVM_GET_XSAVE
export can replace that MXCSR with its initial value. Partial masks preserve
unrequested register contents, and allocation never occurs after native commit.

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
Subleaf 1 EAX can expose XSAVEOPT and XSAVEC (bits 0 and 1); EBX is calculated
from virtual XCR0 and virtual compacted component alignment. XGETBV(1),
XSAVES and XFD remain rejected, as do nonzero XSS and nested VMX/SVM or SGX.
Other CPUID features
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
decoder permits XGETBV(0), XSETBV(0), XSAVE, XRSTOR, XSAVEOPT and XSAVEC in
this mode; ordinary #UD handling is not broadened to arbitrary instructions.
Both the REX.W and ordinary forms are handled. XGETBV(1) and
invalid indices raise #GP(0); invalid XCR0 writes preserve the previous
value. XSETBV at CPL3 raises #GP(0), except that virtual OSXSAVE=0 raises
#UD first. XSAVE-family instructions work at CPL0 or CPL3, check the virtual
CPUID feature and OSXSAVE before CR0.TS (#NM), and ignore CR0.EM. Invalid
headers/MXCSR and misalignment raise #GP(0); alignment checking also uses
the architecturally permitted #GP choice at CPL3. Illegal LOCK prefixes and
unadvertised optional instructions raise #UD. Prefix encodings for PTWRITE
and CLWB are not mistaken for XSAVE/XSAVEOPT. AVX/AVX-512 vector instructions,
XSAVES and XRSTORS still raise #UD. This remains an experimental CPU model.

``kernel-work/tests/hbt-cpuid-live.c`` in the parent repository executes on
the actual KVM backend inside the disposable nested test VM. For each of
the Intel/AMD layouts it tests all 256 low-byte XCR0 values, malformed
profiles and unchanged state on rejection, CPL0/3, 32/64-bit execution,
CR4 read/write shadowing, dynamic CPUID, per-vCPU isolation, native XSAVE
boundaries, and continued trapping of vector instructions.

``hbt-xsave-live.c`` executes 768 mask/format round-trips across Intel/AMD
layouts and 32/64-bit modes. It also checks actual SSE updates before save,
SSE execution after restore, CPL3, REX.W and address-size handling, init state,
MXCSR retention through the native ABI, optional feature gates, dynamic
compacted sizes, XCR0 filtering, #UD/#NM/#GP/#SS/#PF, segment limits,
uncommitted state on failed restore, partial save writes, vCPU isolation and
dirty logging. Protected 16-bit, real mode and VM86 execution have not been
validated by this live harness.

Remaining integration work
--------------------------

Persistent per-vCPU software storage and a versioned VMM transfer interface
are implemented by ``KVM_CAP_HBT_X86_XSTATE_STORAGE`` (see ``hbt.rst``). The
storage API, codec and guest execution adapter are connected and tested.

* AVX/AVX-512 execution with correct mixed native/software state changes.
* XSAVES/XRSTORS and supervisor-state support if the target CPU profile needs them.
* QEMU import/export, guest context-switch and signal tests, then migration.

Specification: Intel SDM Volume 2D, XSAVE/XSAVEOPT/XSAVEC/XRSTOR, and Volume 1,
Chapter 13 (XSAVE-managed state).
