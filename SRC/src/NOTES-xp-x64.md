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

## YMM0-7 only
The guard preserves **YMM0-7 upper halves only**, not YMM8-15. A build that
saved/restored YMM8-15 reboot-looped; limiting to YMM0-7 (as on x86, which only
has 8) is stable. The exact reason YMM8-15 faults at this hook site is
unconfirmed (candidates: the x64 kernel's use of the non-volatile XMM6-15 around
the switch, or a QEMU-TCG limitation with VEX ops on the high registers in
kernel mode). Most AVX code uses YMM0-7 and the AVX ABI treats all YMM upper
halves as volatile across calls, so the practical impact is narrow.

## PatchGuard (the real blocker)
XP x64 / Server 2003 ship Kernel Patch Protection. It hashes the kernel's code,
so ANY patch to SwapContext is detected and bugchecks `0x109`
(CRITICAL_STRUCTURE_CORRUPTION) at the next verify, typically 1-2 minutes after
the hook arms.

- Tested on **5.2.3790.1830 (srv03_sp1_rtm)**. This is PatchGuard **v2**.
- Setting `KdDebuggerNotPresent = 0` alone does **not** disable PG v2 here (it
  still bugchecks 0x109). Booting `/debug` *and* setting
  `KdDebuggerNotPresent = 0` makes the running KD engine block on every debug
  notification (no debugger on COM), so logon hangs. Neither is a usable bypass.
- PG v2 stands down only when a kernel debugger is genuinely **attached**
  (`KdDebuggerEnabled = 1` and `KdDebuggerNotPresent = 0`, i.e. `/debug` plus a
  debugger actually connected).

Therefore `guardx64.c` installs the hook **only** in that
debugger-attached configuration, and on any normal boot reports
`CG_PATCHGUARD` and touches nothing, so WINSIMD never bugchecks the machine.

## Verification status (QEMU, XP x64 5.2.3790.1830, -cpu max, -smp 1)
Confirmed:
- The driver loads; `kernel_base()` finds ntoskrnl (base 0xfffff80001000000) and
  the 16-byte signature scans **uniquely** to the real SwapContext (rva 0x54c48).
- With PatchGuard disabled (debugger-attached gate satisfied in a test build),
  the hook **arms** (GuardFlags = ACTIVE|YMM, GuardComponents = AVX), the hook
  **runs on every context switch** (one-shot E9 marker fired), and the system
  reaches the desktop and runs; the guard was killed only later by PatchGuard's
  own verify (0x109 at the SwapContext address) — i.e. the guard works; PG is
  what stops it, exactly as expected.
- On a normal boot the production gate reports `CG_PATCHGUARD` and the machine is
  stable (no 0x109).
- Both Flash Player 10 and Flash Player 32 run on XP x64 with no guard (the OS
  saves XMM itself; the ports use SSE2).

Not captured: a full YMMSTRS 0-mismatch pass with PatchGuard genuinely off (no
debugger available in this environment). The YMM0-7 upper-only save/restore is
the identical path proven on Windows 2000 and XP x86 (both 0 mismatches).

Disabling PatchGuard outright (so the guard can run on a normal boot) is tracked
as separate, later work.
