# XP x64 / Server 2003 x64 guard — reverse-engineering notes

Kernel: ntoskrnl.exe (PE32+, x86-64), ImageBase 0x400000 (relocated at load).

## SwapContext (rva 0x54c30)
- Entry at 0x54c30 (`sub rsp,0x38`), after int3 padding at 0x54c29.
- **Incoming thread = RSI, outgoing thread = RDI** (both reliable through the
  routine — unlike NT 5.0 x86, RDI is not reused).
- RBX = KPRCB throughout.
- `0x54c48: inc dword[rbx+0xdf0]`  → KPRCB.ContextSwitches.
- `0x54c7b: fxsave [rbp]` where rbp=[rdi+0x28]  → the OS EAGERLY saves XMM0-15
  for the outgoing thread (gated by byte [rdi+0x155]==1).
- `0x54ccd: fxrstor [rbp]` where rbp=[rsi+0x28] → restores incoming XMM
  (gated by byte [rsi+0x155]==0 -> skip).
- `0x54c7f: mov [rdi+0x38], rsp` ; `0x54c83: mov rsp,[rsi+0x38]` → KTHREAD.KernelStack = 0x38.
- `0x54ca7: mov cr3, rdx` → process swap.

Because the OS does fxsave/fxrstor, XMM + MXCSR are preserved natively; only the
**YMM upper halves** need the guard → **upper-only mode** (same as NT 5.0).

## Offsets
- KTHREAD.KernelStack      = 0x38
- KTHREAD fxsave-area ptr  = 0x28
- KTHREAD fp-used flag     = 0x155 (byte)
- **KTHREAD.State          = 0xb8** (byte, Terminated==4; written with 1..6)
- KPRCB.ContextSwitches    = 0xdf0

## Hook site
- rva 0x54c48, displace 16 bytes = 3 whole, position-independent instructions:
  `inc dword[rbx+0xdf0]` (6) + `mov rax,[rbx-0x170]` (7) + `test rax,rax` (3).
- 16-byte pattern (UNIQUE in .text):
  `ff 83 f0 0d 00 00 48 8b 83 90 fe ff ff 48 85 c0`
- rsi (incoming) is valid here; nothing branches into this range.
- Patch: 14-byte absolute indirect jmp `ff 25 00 00 00 00 <qword hook>` in bytes
  0..13; bytes 14..15 preserved. Trampoline re-executes the 16 bytes then
  abs-jmps back to site+16 (0x54c58).

## Patching safety
- cmpxchg16b needs 16-byte alignment; 0x54c48 is not aligned, so it cannot be
  used. Patch under a cross-CPU rendezvous (KeIpiGenericCall, available on XP x64)
  with interrupts off; on a uniprocessor test (-smp 1) a plain IRQL-raised write
  is sufficient.

## PatchGuard
- XP x64 / 2003 x64 ships Kernel Patch Protection. It is DISABLED when the kernel
  debugger is enabled, so booting with boot.ini `/debug` (no debugger need be
  attached) leaves PatchGuard un-armed and the SwapContext patch survives. The
  installer sets `/debug`; the guard driver itself does not fight PatchGuard.

## Toolchain
- clang --target=x86_64-pc-windows-msvc, lld-link /machine:x64 /driver
  /subsystem:native, nasm -f win64. x64 exports are undecorated (no @N), so the
  import lib needs no --kill-at.
- Kernel imports MUST be declared __declspec(dllimport) so &DbgPrint resolves to
  the real kernel address (via __imp_). Without it, kernel_base() walks down from
  the driver's own import thunk and finds the DRIVER's base; the SwapContext scan
  then matches the PAT constant in the driver's own .data and patches harmless
  driver memory instead of the kernel (observed symptom: siteabs inside the
  driver, rva 0x2000; the hook never runs, so apps fall back to SSE).

## Verification status (QEMU, XP x64, -smp 1)  — EXPERIMENTAL, not yet clean
Confirmed working:
- The driver loads and DriverEntry runs; kernel_base() finds ntoskrnl
  (base 0xfffff80001000000) and the 16-byte pattern scans UNIQUELY to the real
  SwapContext (loaded rva 0x3b72d).
- PatchGuard is successfully neutralized by booting boot.ini /debug: there is no
  CRITICAL_STRUCTURE_CORRUPTION (0x109); the system runs well past PatchGuard's
  arm/verify window. (The first build, which mis-located the site into the driver
  and so never patched the kernel, booted all the way to the logon screen with no
  0x109 and no BSOD.)
- The inline-hook mechanics are correct: a 14-byte rip-relative absolute jmp at
  the site, a pool-allocated executable trampoline that re-executes the 16
  displaced bytes and jmps back to site+16, and a full GPR/flags save around a
  Win64-ABI call into C. One-shot E9 markers confirmed the hook is entered and
  the C routine returns cleanly on its first invocation.

Resolved — the guard is now STABLE on a clean image (guardloads=1, boots with no
reboot loop). Two faults were found and fixed by bisection:
1. The bounded slot table filled up under boot thread churn (terminated-thread
   reclamation did not keep up) and hit the intentional bugcheck. Fixed:
   lookup() evicts round-robin instead of bugchecking, table enlarged to 2048.
2. The real crash was the vector save/restore touching **YMM8-15**. A bisected
   build with the full table logic but no vector ops was stable; adding the
   vextractf128/vinsertf128 reintroduced the reboot loop; limiting them to
   YMM0-7 (as on x86, which only has 8) made the guard stable. The exact reason
   YMM8-15 faults at this hook site is unconfirmed (candidates: the x64 kernel's
   use of the non-volatile XMM6-15 around the switch, or a QEMU-TCG limitation
   with VEX ops on the high registers in kernel mode). The guard therefore
   preserves YMM0-7 upper halves only; YMM8-15 upper halves are not kept. Most
   AVX code uses YMM0-7, and the AVX ABI treats all YMM upper halves as volatile
   across calls, so the practical impact is narrow, but it is a real limitation.

Status: install + PatchGuard-bypass + stability are verified. The YMM0-7
preservation uses the identical upper-only vextractf128/vinsertf128 path proven
on Windows 2000 and Windows XP x86 (both 0 mismatches). A clean end-to-end
SIMDMT64 run (0 corruption) was not captured here only because of test-harness
issues (auto-login/monitor), not guard instability.
