HBT virtual XSTATE implementation
================================

The first implementation is a host-independent XSAVE64/XRSTOR64 codec in
``arch/x86/kvm/hbt_xstate.[ch]``. It is compiled into KVM but is not yet called
by the guest instruction emulator. It does not enable AVX-512 CPUID bits,
change CR4/XCR0, or make an AVX-512 VM migratable to an AVX2 host.

The normalized state holds x87/SSE, YMM_Hi128, opmask, ZMM_Hi256 and Hi16_ZMM.
The caller supplies the virtual CPUID.0D layout and MXCSR mask. Standard
offsets may differ from the host; compacted offsets are calculated from
XCOMP_BV and the supplied component alignment flags. Only user components
0, 1, 2, 5, 6 and 7 are supported. Other features, supervisor state, and
non-REX.W legacy representations require subsequent implementation.

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
Native AVX-512 differential execution is still required on a capable machine.

Remaining integration work
--------------------------

* Persistent per-vCPU software state and a versioned VMM transfer interface.
* Separation of virtual XCR0/CPUID.0D from hardware XCR0/FPU buffer layout.
* A VMX test mode using CR4 read shadow with actual GUEST_CR4.OSXSAVE=0,
  plus #UD dispatch for the XSAVE family, XGETBV and XSETBV. This also traps
  AVX/AVX2 and cannot use the current guest AVX2 helpers unchanged.
* Instruction decoding, guest-memory fault delivery, XSAVES/XRSTORS, native
  FPU synchronization and correct mixed AVX/AVX-512 state changes.
* QEMU import/export, guest context-switch and signal tests, then migration.

Specification: Intel SDM Volume 2D, XSAVE/XSAVEOPT/XSAVEC/XRSTOR, and Volume 1,
Chapter 13 (XSAVE-managed state). The codec's transaction contract must not
be mistaken for complete instruction emulation.
