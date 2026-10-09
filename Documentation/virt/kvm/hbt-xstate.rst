HBT virtual XSTATE implementation
================================

The implementation has a host-independent XSAVE/XRSTOR codec in
``arch/x86/kvm/hbt_xstate.[ch]``, software AVX-512 register storage, and an
opt-in VMX path for virtual CPUID/XCR0 and guest XSAVE-family emulation.
``hbt_xstate_emulate.c`` connects the live native FPU to the codec and software
state. A bounded integer VEX/EVEX executor and the parent repository's QEMU
integration now support native AVX-512 -> AVX2/HBT live migration of the
validation guest, including re-export after software execution.

Bounded EPTP-switching experiment
--------------------------------

``kvm_intel.hbt_eptp_probe=1`` enables the private ``KVM_HBT_EPTP_PROBE``
vCPU ioctl declared in ``linux/kvm_hbt.h``. It defaults off and requires Intel
EPT, VPID and VMFUNC function 0 support. It is a test RAM backend: CONFIG copies
at most 512 pages at GPA zero into accounted kernel-owned pages, with at most
16 overlays for a second execution view. It does not pin memslot PFNs, accept
host physical addresses, or replace normal KVM memory management. Each vCPU
owns an independent image. In-kernel IRQ chips, SMM and nested VMX guests
are excluded. Tests may themselves run under L0 nested virtualization, but
QUERY labels that environment and its performance is not a native measurement.

The two EPT roots and function control are installed only around hardware
entry and restored immediately after hardware exit. All guest exceptions are
intercepted, PML and EPT #VE are disabled during entry, and all exits return
to the test driver without normal instruction-exit emulation. READ returns
one complete copied RAM view; DESTROY releases the private pages and restores
ordinary execution on the next KVM_RUN. Exits use ``KVM_EXIT_HBT_EPTP`` and
``run->internal`` as documented in the UAPI header. Interpret instruction
length, qualification and interruption fields only for exit reasons that
define them. The active view persists across KVM_RUN calls.

The parent repository's ``kernel-work/tests/test-hbt-eptp.py`` supplies known
guest page tables, transition gates and register scratch. It exercises the
existing SSE2 generator and explicitly joins its shadow to canonical XSTATE
at stopped normal-instruction boundaries, then checks guest XSAVE and transfer
to another vCPU. This is not automatic fault/interrupt recovery, a migration
API, or transparent instrumentation of arbitrary guest code. Ordinary guest
XSAVE emulation must run after the probe is destroyed and canonical state
is restored. Production integration still needs mapping lifetime, guest RAM
coherence, precise event handling and automatic state ownership transitions.

Canonical XSTATE representation
------------------------------

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
and CLWB are not mistaken for XSAVE/XSAVEOPT. Unsupported vector instructions,
XSAVES and XRSTORS still raise #UD. This remains an experimental CPU model.

``kernel-work/tests/hbt-cpuid-live.c`` in the parent repository executes on
the actual KVM backend inside the disposable nested test VM. For each of
the Intel/AMD layouts it tests all 256 low-byte XCR0 values, malformed
profiles and unchanged state on rejection, CPL0/3, 32/64-bit execution,
CR4 read/write shadowing, dynamic CPUID, per-vCPU isolation, native XSAVE
boundaries, and continued trapping of unsupported vector instructions.

``hbt-xsave-live.c`` executes 768 mask/format round-trips across Intel/AMD
layouts and 32/64-bit modes. It also checks actual SSE updates before save,
SSE execution after restore, CPL3, REX.W and address-size handling, init state,
MXCSR retention through the native ABI, optional feature gates, dynamic
compacted sizes, XCR0 filtering, #UD/#NM/#GP/#SS/#PF, segment limits,
uncommitted state on failed restore, partial save writes, vCPU isolation and
dirty logging. Protected 16-bit, real mode and VM86 execution have not been
validated by this live harness.

Integer vector execution and migration
--------------------------------------

Query-only KVM_CAP_HBT_X86_INTEGER_VECTOR returns version 1 for the bounded
integer executor. A VMM should require it as well as VIRTUAL_XSTATE to avoid
starting a vector guest on earlier control/storage-only kernels.

The opt-in decoder accepts VPXOR/VPXORD/VPXORQ, VPADDB/W/D/Q,
VPBROADCASTB/W/D/Q and VMOVDQA/VMOVDQU (including EVEX 32/64-bit elements)
in 32/64-bit protected mode. Broadcasts accept vector/memory sources and
the EVEX general-register forms. VEX lengths are 128/256 bits;
EVEX lengths are 128/256/512, with AVX512VL required for the shorter EVEX forms.
Byte/word EVEX arithmetic and broadcasts additionally require AVX512BW.
It validates virtual CPUID, CR0/CR4 and XCR0, supports high registers, opmasks,
merge/zero behavior, broadcast, compressed disp8 and RIP-relative addressing.
Invalid prefixes, unavailable state/features and unsupported encodings trap.
Virtual state availability is checked before CR0.TS (#NM).

Low YMM halves use the live native FPU under fpregs_lock; upper halves and
high registers use the per-vCPU software state. Scalar integer operations
produce the result. Register destinations commit after all required guest RAM
reads succeed, and allocation precedes native register writes. Masked-off
memory lanes are not accessed. Store writes participate in dirty accounting;
completed lanes can remain visible on a later fault. MMIO is unsupported.
The vCPU lock serializes execution with GET/SET state transfer. VEX/EVEX writes
zero bits above their vector length and conservatively mark changed software
components in use; other registers/components are preserved.

The parent repository's QEMU opt-in mode exposes an AMD- or Intel-offset
profile and transfers software state through the existing AVX-512 CPU VMState
subsection. Native XSAVE ioctls receive only components 0..2, while the software
API transfers components 5..7. KVM_GET/SET_XCRS carries the virtual XCR0. Native
source execution, migration without reboot, guest XSAVE of all 32 ZMMs/eight
opmasks, continuing arithmetic and RAM checks, and HBT-to-HBT re-export are
covered by ``test-hbt-avx512-migration.py``. See the parent repository's
``kernel-work/LIVE-MIGRATION.md`` for configuration and limitations.

``hbt-vector-live.c`` runs 480 register/mask/length cases in 32/64-bit modes,
plus narrow VEX zeroing, memory broadcast, masked stores, compressed and
RIP-relative addressing, fault suppression, failed-load state preservation,
and XCR0/CR0/CR4 guards on the actual KVM backend.

Scalar VEX execution
--------------------

The executor also accepts VEX VMOVD/Q (general-register, vector-register and
memory forms), VMOVSS/SD, VMOVUPS/UPD/APS/APD, VAND/ANDN/OR/XORPS/PD,
VZEROUPPER/ALL, scalar VADD/SUB/MUL/DIVSS/SD, VFMADD132/213/231SS/SD,
VCVTSI2SS/SD, VPINSRD/Q, and VINSERTF/I128. Scalar arithmetic uses VEX.L=0;
EVEX scalar rounding/SAE and packed FP arithmetic are not implemented. Other
conversions, FMA families and vector operations remain unsupported.

Scalar stores preflight all covered pages with write permissions before
committing any byte. A fault on the second page therefore leaves the first
page unchanged, unlike the documented per-lane policy for packed stores.

The scalar arithmetic helper runs host SSE (host FMA for fused operations)
under kvm_fpu_get/put with the guest's live MXCSR. It saves and restores all
scratch registers, including native YMM upper halves. Guest memory is read
before acquiring the FPU lock, and software destination backing is reserved
before arithmetic changes MXCSR. A host arithmetic exception is caught by
the kernel exception table; its hardware-generated MXCSR flags are retained,
the destination is not committed and #XM (or #UD with CR4.OSXMMEXCPT clear)
is injected into the guest. FMA requires the guest CPUID feature and native
host FMA. This does not split a fused operation into rounded multiply/add.

The parent repository's ``test-hbt-avx.py`` executes identical VEX bytes in
native and virtual-XSTATE VMs on the same host. It compares registers, memory,
flags, MXCSR and faults in 32/64-bit modes at CPL0/3, including rounding,
NaN payloads, signed zero, DAZ/FTZ, unmasked exceptions, operand aliasing,
reserved encodings and scalar accesses across a missing second page.
EVEX broadcasts separately test high registers, 64-bit masks and fault
suppression. These checks do not establish complete CPU compatibility.

The automatic EPTP experiment has a separate userspace compiler for direct
legacy SSE execution. Its bounded decoder includes moves, integer operations,
scalar and packed arithmetic, conversions and inserts. The kernel dispatch
filter permits those candidates, including four-byte VEX encodings, while
retaining virtual CR0/CR4/XCR0 and AVX512VL guards. The VMM validates each
instruction's features, operands, guest mappings and patch placement before
publishing private code. FMA and other untranslated forms still use this
kernel fallback. The fallback implementation itself does
not solve short-instruction placement or add an EPTP floating-point backend.

Remaining work includes broader AVX/AVX-512 coverage (packed floating point, mask
instructions and gathers/scatters), guest OS context switches/signals, SMP,
disk/device workloads and performance. XSAVES/XRSTORS and supervisor state
remain outside the accepted CPU profile.

Specification: Intel SDM Volume 2, instruction references, and Volume 1,
Chapters 11, 13 and 14 (SIMD exceptions, XSAVE-managed state and AVX/FMA).
