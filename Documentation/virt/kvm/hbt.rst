.. SPDX-License-Identifier: GPL-2.0

Experimental HBT userspace #UD analysis
=======================================

This linux-hbt extension is a private #UD interface, not an upstream KVM ABI.
The definitions in ``include/uapi/linux/kvm_hbt.h`` use private capability and
exit number ``0x48425401``. Instruction decoding and candidate selection stay
in userspace. A separately enabled XOM capability installs a supplied page
image; it does not validate the replacement's instruction semantics.

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

The virtual-XSTATE/EPTP dispatch mode instead captures at most 15 bytes. It
walks each guest virtual page with instruction-fetch permissions, so an
instruction may cross a page boundary without requiring physically adjacent
guest pages. An unavailable following page shortens the window. ``gpa`` still
identifies the first byte only; userspace must translate subsequent virtual
pages separately before installing execution overlays. The ordinary XOM
capture contract above remains unchanged.

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

KVM_CAP_HBT_X86_XOM
------------------

:Architectures: x86 (VMX with EPT execute-only support)
:Type: VM capability, private number 0x48425403
:Parameters: args[0] = KVM_HBT_ABI_VERSION (1); args[1..3] and flags = 0

Enable UD and RETRY first, then XOM before creating the sole vCPU. Unsupported
parameters/backend or missing RETRY return EINVAL; an existing vCPU returns
EBUSY. KVM_CHECK_EXTENSION returns 1 only with EPT execute-only support.
This enables KVM_HBT_INSTALL_XOM, KVM_HBT_TRANSLATE_RW and the generation-checked
page query/update interface below. It does not itself change any guest page.

KVM_CAP_HBT_X86_XOM_UPDATE
--------------------------

:Architectures: x86 (VMX with EPT execute-only support)
:Type: Query-only capability, private number 0x48425404

KVM_CHECK_EXTENSION returns ABI version 1 when the page query/update interface
is available. There is no separate ENABLE_CAP operation; enable XOM as above.
The original INSTALL ioctl remains available for old callers. Its entries do
not carry generations and cannot be updated by this interface.

KVM_CAP_HBT_X86_XSTATE_STORAGE
----------------------------

:Architectures: x86 (VMX, default VM type)
:Type: VM capability, private number 0x48425405
:Parameters: args[0] = KVM_HBT_XSTATE_VERSION (1); args[1..3] and flags = 0

An opt-in experimental storage interface for software-managed AVX-512
components, separate from KVM_GET/SET_XSAVE and their host CPUID layout.
KVM_CHECK_EXTENSION returns 1 when supported. Enable it before creating any
vCPU; existing vCPUs cause EBUSY, unsupported arguments cause EINVAL. It does
not depend on UD/RETRY/XOM or limit the number of vCPUs. It cannot be disabled.

This capability stores data only. It does not advertise AVX-512, change
guest or hardware XCR0/CR4, execute instructions, or synchronize native FPU
registers. It is not a promise that a native AVX-512 VM can run or migrate.
The virtual-XSTATE execution mode below connects storage to guest execution
and VMM state transfer; storage alone does not enable that mode.

KVM_CAP_HBT_X86_VIRTUAL_XSTATE
----------------------------

:Architectures: x86 (VMX, default VM type, native AVX2)
:Type: VM capability, private number 0x48425406
:Parameters: args[0] = KVM_HBT_VIRTUAL_XSTATE_VERSION (1); args[1..3] and flags = 0

Experimental virtual CPUID/XCR0 control path. KVM_CHECK_EXTENSION returns 1
when supported. Enable before creating any vCPU; existing vCPUs cause EBUSY.
Invalid arguments, unsupported backend and combining with HBT UD/RETRY/XOM
cause EINVAL. It cannot be disabled and also enables XSTATE_STORAGE.

The VMM installs a validated virtual CPUID.0D profile with user-state mask
0xe7 via KVM_SET_CPUID2. KVM_GET_SUPPORTED_CPUID remains host-based.
XGETBV/XSETBV and KVM_GET/SET_XCRS use virtual XCR0; hardware XCR0 and the
native KVM_GET/SET_XSAVE ABI remain limited to x87/SSE/YMM. VMX forces actual
CR4.OSXSAVE=0 while preserving its guest-visible read shadow.

Guest XSAVE/XRSTOR, XSAVEOPT and XSAVEC are emulated with native FPU and
software AVX-512 state synchronization. A bounded integer VEX/EVEX executor is
available when INTEGER_VECTOR reports version 1. This is not a complete
AVX-512 guest CPU.
See ``hbt-xstate.rst`` for the required CPUID profile, semantics and tests.

KVM_CAP_HBT_X86_INTEGER_VECTOR
------------------------------

:Architectures: x86 (VMX, default VM type, native AVX2)
:Type: Query-only capability, private number 0x48425407

KVM_CHECK_EXTENSION returns KVM_HBT_INTEGER_VECTOR_VERSION (1) when the bounded
integer executor described in ``hbt-xstate.rst`` is available. Enable
VIRTUAL_XSTATE to use it; there is no separate ENABLE_CAP operation. This check
lets VMMs reject older storage/control-only kernels before starting a guest.
It does not promise the complete AVX2, AVX512F or AVX512VL instruction sets.
The newer scalar VEX operations documented in ``hbt-xstate.rst`` extend this
executor; version 1 remains the original minimum subset, not a feature bitmap
for every added instruction.

KVM_HBT_GET_XSTATE / KVM_HBT_SET_XSTATE
-------------------------------------

:Type: vCPU ioctls, private numbers 0xee / 0xef
:Parameters: struct kvm_hbt_xstate (1664 bytes)

GET is output-only; SET is input-only. The structure has a 64-byte header
(version, size, xfeatures, xinuse and five reserved u64 fields), then 64 bytes
of opmask, 512 bytes of ZMM0-15 upper halves, and 1024 bytes of ZMM16-31.
It is neither a guest XSAVE area nor a native FPU image. Its register byte
arrays use x86 little-endian lane ordering. Set version=1, size=1664,
xfeatures=0xe0, xinuse to a subset of 0xe0, and reserved fields to zero.
SET zeroes the backing bytes of components absent from xinuse. GET returns
this canonical form. No user pointers, FS bases or guest addresses are stored.

Each vCPU starts with zero registers and xinuse=0. Storage survives INIT,
like native AVX-512 user state; RESET and destruction discard it. A VMM that
implements a machine reset by writing individual architectural registers
must explicitly SET an initial software state too. vCPU locking serializes
GET/SET with execution. SET validates and copies everything before publication;
allocation, validation or copy failures leave existing state untouched. GET
copy failures have no state effect and their output must be discarded.

Without opt-in, or for nested/protected guest state, the ioctls return
EOPNOTSUPP. Invalid fields return EINVAL, user-copy failures EFAULT, and
allocation failures ENOMEM. They do not require a pending HBT #UD request.

KVM_HBT_GET_XOM_PAGE
--------------------

:Type: vCPU ioctl, private number 0xec
:Parameters: struct kvm_hbt_xom_page (48 bytes, input/output)

Set version=1, reserved=0, request_id to the pending unacknowledged #UD ID,
gpa to the aligned faulting page, and generation=0. original_addr and
current_addr point to writable userspace buffers of 4096 bytes each. With
the fault context and fetch GPA revalidated, the ioctl copies the first
original page, current live replacement and generation under the XOM lock.
Generation zero means no translation, in which case both images match.
A legacy entry returns EOPNOTSUPP. Copy errors return EFAULT and do not
acknowledge the request or change the guest. Treat any failed output as invalid.

KVM_HBT_UPDATE_XOM
------------------

:Type: vCPU ioctl, private number 0xed
:Parameters: struct kvm_hbt_xom_update (56 bytes, input only)

Set version=1, reserved=0, request_id and gpa as above, expected_generation
from GET_XOM_PAGE, and original_addr/current_addr/replacement_addr to readable
4096-byte images. expected_generation=0 requests the first installation and
requires original=current. The captured instruction suffix is checked against
original, which is also the source for later #UD snapshots on translated pages.

The operation compares the generation, saved original and full current image
against a pinned writable RAM page under the XOM lock. Mismatched generation
or images return ESTALE. Invalid alignment, generation ordering or RAM slot
returns EINVAL; unsupported pinning returns EOPNOTSUPP. All allocation and
copy failures precede publication and leave guest bytes and completion intact.

Success preserves the first original, installs the complete replacement,
sets generation=request_id, invalidates SPTEs and acknowledges RETRY. There
is no userspace output copy after publication and no separate completion is
needed. The VMM may compose multiple helpers in this replacement. A guest
write restores the first original and retires every helper/generation on the
page; an update with a retired generation returns ESTALE even if bytes happen
to match again. Request IDs survive vCPU reset. The same single-vCPU,
stable-private-RAM and writer-exclusion contract as INSTALL_XOM applies.

KVM_HBT_TRANSLATE_RW
--------------------

:Type: vCPU ioctl, private number 0xeb
:Parameters: struct kvm_hbt_translation (32 bytes, input/output)

Set version=1, reserved=0, physical_address=0, request_id to the pending #UD
ID, and linear_address to a page-aligned guest data address. The ioctl
requires the XOM capability and an unacknowledged request with unchanged
fault context. It walks guest page tables with write access at the current
CPL, including the user-access check at CPL3, and returns physical_address.
It does not inject a page fault, acknowledge the request or install code.
The page walk can update page-table accessed/dirty bits.

The caller must validate the returned GPA against writable ordinary RAM and
keep mappings/content stable. Translation alone does not pin guest page
tables, establish ownership or guarantee a future access will succeed.
The cooperative 64-bit Linux demo uses this to validate a separate resident
data page; its code GPA is captured with instruction-fetch permissions.
The ordinary KVM_TRANSLATE system-access walk is insufficient for this check.

Invalid alignment or nonzero input physical_address returns EINVAL; an
inaccessible mapping or user-copy failure returns EFAULT. A mismatched ID or
changed fault context returns ESTALE; an acknowledged request returns
EALREADY. Failure leaves the request available for normal FALLBACK completion.

KVM_HBT_INSTALL_XOM
------------------

:Type: vCPU ioctl, private number 0xea
:Parameters: struct kvm_hbt_xom_install (40 bytes)

Set version=1, reserved=0, request_id to the pending #UD ID, and gpa to its
aligned 4 KiB page base. original_addr and replacement_addr each point to
4096 readable userspace bytes. The VMM must not pre-write the guest page.

The ioctl revalidates the saved fault context and instruction-fetch GPA,
compares the captured suffix with the supplied original, then compares the
entire original with live RAM through a pinned writable mapping. Read-only,
guest_memfd and non-pinnable mappings are rejected. It allocates all state
before installing the full-page original and replacement, zaps stale SPTEs,
and acknowledges RETRY. The next KVM_RUN executes at unchanged RIP. Every
error leaves guest bytes and pending completion unchanged; no separate
KVM_HBT_COMPLETE is needed after success.

The translated page uses EPT R=0/W=0/X=1. Instruction fetch sees replacement
bytes; emulated CPU data reads see the saved original. A CPU write restores
the entire original before the write, removes all translation metadata for
the page, and invalidates its SPTEs. Same-value writes and writes outside the
patched ranges invalidate as well. Emulated standard writes, operand writes
and compare/exchange follow the same rule. Later execution sees the guest's
new bytes and may cause a fresh #UD analysis. A partial restore failure keeps
the original and XOM metadata; the fault/emulation returns an error instead
of allowing the guest write to continue.

This is a single-vCPU, private-RAM prototype. The VMM must exclude DMA,
userspace writers, aliases and memslot changes throughout the translation's
lifetime, including reset/reinitialization. Such writers bypass EPT and are
not intercepted. VM teardown frees the saved originals; migration and reset
of an installed translation have no state protocol. Keep writable helper
data on a different page. Only data accesses supported by KVM's instruction
emulator are covered; arbitrary SIMD reads and transparent guest OS integration
are not promised. A cooperative Linux process with resident private pages is
tested, but fork/remapping and architectural ZMM signal/XSAVE state are not
virtualized. Timing/debugger observability is not hidden.

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

The ioctls return EOPNOTSUPP when the VM has not enabled the capability, and
ENOENT when no request is pending. A mismatched ID returns ESTALE. Invalid
version, buffer size, action or reserved field returns EINVAL. User-copy
failures return EFAULT. Repeating a successful acknowledgment before KVM_RUN
returns EALREADY. An invalid request does not consume or change pending state.
RETRY without its separate capability returns EOPNOTSUPP; RETRY with a replaced
fault context returns ESTALE. Undefined action values return EINVAL.

INSTALL_XOM additionally returns EOPNOTSUPP without its own capability or
without an ordinary pinned page, ESTALE for changed context/fetch translation
or original bytes, EINVAL for a misaligned/wrong page or unsupported memslot,
EBUSY for an existing XOM entry, and ENOMEM for allocation failures. A failed
installation can be followed by normal FALLBACK completion.

Bounded EPTP experiment
----------------------

``kvm_intel.hbt_eptp_probe=1`` enables the laboratory-only
``KVM_HBT_EPTP_PROBE`` vCPU ioctl. CONFIG/READ copy a fixture of at most
512 pages with at most 16 helper overlays. QUERY reports version/limits and
whether execution is nested; its output ``view`` is the currently selected
view. DESTROY drops the private views. Reserved fields must be zero.

``KVM_CAP_HBT_X86_EPTP_DISPATCH`` additionally allows CPL3 #UD analysis/retry
with virtual XSTATE. Enable virtual XSTATE first, then this capability before
creating vCPUs. It restricts the VM to one vCPU. It does not enable the old
cooperative XOM interface, widen instruction semantics, or provide migration
serialization for pending analysis requests.

LIVE_CONFIG bounds a contiguous user RAM allocation at GPA 0 by ``nr_pages``
(up to 1 GiB) but maps only the listed pages. Each live-map entry supplies a
GPA page number, N permissions in bits 2:0, H permissions in bits 10:8, and
optional N/H image addresses. A nonzero image address is copied to a private
accounted page; a zero address with nonzero permissions pins the corresponding
user RAM page. QUERY's ``KVM_HBT_EPTP_SHARED_OVERLAY`` output flag specifies that
identical nonzero image pointers share one private copy across maps and views.
Each alias retains its own EPT permissions and owns a page reference. This
allows a helper to read saved gate registers without a userspace round trip.
Unlisted GPAs are absent. Private pages may occupy otherwise
unused GPAs below 1 GiB. Duplicate entries, write-without-read permissions,
unknown bits and out-of-range RAM references are rejected. READ_MAPS copies
selected private pages back to userspace using page/destination pairs in the
old overlay structure. It requires zero ``nr_pages``/``image_addr`` and view
0 or 1. READ of a live configuration is rejected.

Both EPTs and the VMFUNC list are owned by the kernel. Original RAM references
are obtained with GUP, never supplied as host physical addresses. A failed
configuration releases every acquired pin and allocation. Teardown marks
pinned RAM dirty and unpins it. This is not a memslot/MMU-notifier backend;
the caller must keep RAM and mappings fixed, stop external memory writers,
and discard the views before changing memory or saving CPU state.

Every private hardware exit reports ``KVM_EXIT_HBT_EPTP`` and raw VMX reason,
active view, qualification, instruction length, interrupt information and RIP
in ``internal``. Ordinary VMCS controls/EPT are restored before userspace runs.
Userspace must normalize helper registers and shadow vectors before allowing
ordinary KVM execution. Guest exception emulation is deliberately bypassed
until then. VMX nesting and SMM are rejected. An in-kernel IRQ chip requires
``kvm_intel.enable_apicv=0`` so posted interrupts cannot expose helper state.

Live entries use a private VMX preemption timer bounded to 100 microseconds
of guest execution. Pending external IRQ injection is canceled for that
slice and requeued after its exit, before ordinary guest execution can resume.
Other pending entry events produce the synthetic suberror
``KVM_HBT_EPTP_PENDING_EVENT`` without entering the guest. The private timer
must not be interpreted as an expired guest LAPIC timer. With ``ndata >= 6``,
``internal.data[5]`` requests canonical recovery for a pending/deferred guest
event. A timer exit with this field zero may resume the existing private views;
once an IRQ has been deferred, recovery is required before further slices.
This avoids recompiling an otherwise uninterrupted loop at every timer exit.
These mechanisms
support a bounded experiment, not arbitrary SMP, DMA, SMI, debug, reset or
migration activity while a view is active.
