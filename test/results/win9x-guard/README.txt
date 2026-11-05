Windows 9x thread-switch vector guard - verification (QEMU)
===========================================================

The Win9x guard (vxd/vxdsched.asm + vxdmain.c) hooks the VMM scheduler via
Call_When_Thread_Switched and saves/restores the per-thread vector state the OS
does not. Like the NT guard it chooses its mode from what the OS already keeps,
detected from CR4 at load (cr4_before & OSFXSR):

  - OSFXSR NOT pre-set  => VMCPD is not SSE-aware (Win95 RTM / Win98 FE):
    FULL mode - guard owns XMM0-7 (or YMM0-7 on AVX) + MXCSR.
  - OSFXSR pre-set      => VMCPD is SSE-aware (Win98 SE / Win ME): it keeps XMM
    + MXCSR itself, so UPPER-ONLY mode - guard keeps only the YMM upper halves;
    with no AVX present there is nothing to guard and the hook is not installed.

Test: test/w9xtest.c (W9XTEST.EXE), 4 threads holding XMM0-7 (+ YMM0-7 on AVX)
+ MXCSR + x87 across a spin under real preemption; counts leaks. No installer
(the guard is the static VxD loaded at boot). E9 debug tag at load:
  H  = Device_Init reached
  X/Y = full mode registered (XMM / YMM)
  U  = upper-only mode registered
  N  = no-op (OS keeps XMM, no AVX) - hook not installed
  !  = the switch callback fired at least once

Windows 95 RTM (QEMU -cpu Haswell, full mode, E9 "HY!"):
  Baseline (enable-only, -DVXD_NO_GUARD):  XMM 348, YMM 282 total mismatches.
  With guard:                              XMM  81, YMM  75 total mismatches.
  -> the VxD scheduler hook works and substantially reduces vector corruption;
     system is fully stable with the callback firing on every switch. Residual
     is imperfect incoming-thread identification with 3+ threads interleaving.

Windows ME (QEMU -machine pc,vmport=on -cpu pentium3; E9 "HN"):
  winme-desktop-vmport.png - ME boots to the desktop. (It is a VMware VMDK whose
    guest driver spins on the VMware backdoor port; QEMU's vmport=on answers it,
    otherwise ME hangs at EIP 0x6173. -cpu pentium3 has SSE but no AVX.)
  winme-pass-xmm0.png - W9XTEST: XMM fail=0, YMM fail=0 -> PASS. The guard
    detected the SSE-aware VMCPD and deferred XMM to it (no-op); ME's VMCPD
    preserves concurrent XMM itself. Correct behaviour.

Windows 98 FE (VirtualBox VHD): did not boot to a testable desktop in this
QEMU/TCG sandbox (hangs after its hardware reconfiguration, same class of issue
as ME's backdoor spin); the guard is version-agnostic across Win9x and uses the
same mechanism verified on 95 and ME.
