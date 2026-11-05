# WINSIMD 1.2

A CPU SIMD-enabling driver for **Windows NT 3.1 / 3.5x / 4.0 / 2000 / XP** (i386),
**Windows XP x64** (amd64) and **Windows 95 / 98 / ME**. When the processor
supports them, it makes MMX / SSE..SSE4.2 / AVX / AVX2 / AVX-512 usable without
conflicting with what the OS already does. **New in 1.2**, it also keeps those
registers correct across thread switches where the OS does not.

| Target | File | Format |
|---|---|---|
| NT 3.1 → XP (x86) | `out/i386/WINSIMD.SYS` | PE kernel driver, native 3.10 |
| XP x64 | `out/amd64/WINSIMD.SYS` | PE32+ kernel driver |
| Windows 9x | `out/9x/WINSIMD.VXD` | LE static VxD |
| Installer (NT) | `out/<arch>/WINSIMD.EXE` | console |
| Status viewer | `out/<arch>/WINSINFO.EXE` | GUI |

See README-UPSTREAM-1.1.md for installation, the enable policy and the 1.1
test history. CHANGELOG.txt lists every 1.2 change with its test results.

## Why 1.2

Enabling a register file (CR4.OSFXSR, CR4.OSXSAVE + XCR0) only stops the
instructions from faulting. The OS must also save the registers when it
switches threads. Here is who saves what:

| Register file | NT 3.1–4.0 | 2000 / XP | 95, 98 first ed. | 98 SE / ME | XP x64 |
|---|---|---|---|---|---|
| x87 / MMX | OS | OS | OS | OS | OS |
| XMM + MXCSR | **WINSIMD 1.2** | OS | **WINSIMD 1.2** | OS | OS |
| YMM upper halves | **WINSIMD 1.2** | **WINSIMD 1.2** | **WINSIMD 1.2** | **WINSIMD 1.2** | WINSIMD 1.2 † |
| AVX-512 opmask / ZMM | **WINSIMD 1.2** | **WINSIMD 1.2** | **WINSIMD 1.2** | **WINSIMD 1.2** | — ‡ |

With 1.1, a register file the OS does not save leaked between threads. A
60-second, three-thread test on NT 3.1 failed about 680 of 4300 checks per
thread. With 1.2 no check fails.

† **XP x64 / Server 2003 x64** saves XMM/MXCSR itself but not the YMM upper
halves. WINSIMD 1.2 preserves YMM0–7 upper halves there by hooking the kernel's
`SwapContext` (see [How the XP x64 YMM guard works](#how-the-xp-x64-ymm-guard-works)).
Because that is a code patch, **PatchGuard must be disabled** (a kernel debugger
attached) for it to arm; otherwise WINSIMD leaves the kernel untouched and the
guard stays off. SSE/XMM apps — including the Flash Player ports — need no guard
on XP x64, as the OS saves XMM itself.

‡ The amd64 guard preserves YMM only, not AVX‑512 ZMM/opmask.

## How the context guard works

`src/ctxguard.c`, `src/ctxguard_x86.asm`; NT glue in `src/guard_nt.c`, 9x
glue in `vxd/vxdmain.c`.

1. **Lazy-switch trap.** All supported schedulers switch the FPU lazily: a
   thread that does not own the FPU runs with CR0.TS set. Its first x87, MMX,
   SSE, AVX or AVX-512 instruction raises #NM (vector 7). WINSIMD hooks vector
   7 in front of the OS handler.
2. **Swap.** If this CPU's registers belong to another thread, the guard
   stores them in that thread's slot and loads the current thread's slot.
   A thread with no slot gets zeroed registers and MXCSR = 1F80h. Execution
   then continues into the OS handler, which swaps x87 as before.
3. **Components.** The guard saves only what the OS does not, with
   XSAVE/XRSTOR and exactly that component mask, or with MOVUPS + STMXCSR
   when only XMM is needed and XSAVE is unavailable.
4. **Multiprocessor.** A thread may continue on another CPU while its values
   are still in the old CPU's registers. The new CPU detects this and asks the
   old CPU, by an NMI through the local APIC, to store them. The old CPU's NMI
   hook does so and releases ownership. Real NMIs still reach the OS. No CPU
   waits while holding the table lock, and a waiting CPU serves requests
   addressed to itself.
5. **Thread identity and lifetime.**
   * Identity on NT: `KPCR.Prcb->CurrentThread`. Selector 30h, `[fs:20h]+4`;
     the same on every x86 NT.
   * Identity on 9x: VMM `Get_Cur_Thread_Handle`.
   * Release on thread exit: `PsSetCreateThreadNotifyRoutine` (NT 4.0+) or
     the `Destroy_Thread` control message (9x).
   * NT 3.1/3.5x have no notification; there the least recently used unloaded
     slot is reused.

Nothing depends on a kernel build, timestamp or code offset. The only layout
facts used are architectural or identical on every x86 NT:
* the KPCR selector 30h;
* `KPRCB+4` CurrentThread;
* `KPRCB+14h` SetMember.

## How the XP x64 YMM guard works

`src/guardx64.c`, `src/guardx64.asm`. See `src/NOTES-xp-x64.md` for the
reverse-engineering.

The x86-64 kernel (XP x64 / Server 2003 x64) switches the FPU **eagerly** —
`SwapContext` does `fxsave`/`fxrstor` unconditionally — so there is no #NM trap
to piggy-back on, and XMM/MXCSR are preserved by the OS. The AVX **YMM upper
halves are not** saved, though, so concurrent AVX threads corrupt each other's
YMM across a switch. WINSIMD preserves them with a direct `SwapContext` hook:

1. **Locate the kernel and the hook site dynamically.** The running `ntoskrnl`
   base is found from WINSIMD's own `DbgPrint` import; the `SwapContext` site is
   found by scanning for a unique 16-byte signature and is used only if it
   occurs **exactly once**. No fixed address, RVA, build stamp or struct offset
   is used; per-CPU identity is the `KPRCB` pointer, registered on first sight.
2. **Hook it** with a 14-byte absolute `jmp` and a trampoline that re-runs the
   displaced bytes, installed under `KeIpiGenericCall` (all CPUs synchronised).
3. **Preserve YMM0–7 upper halves per thread** (`vextractf128`/`vinsertf128`),
   leaving the OS's XMM state alone. Slots are freed on thread exit via
   `PsSetCreateThreadNotifyRoutine`. The hot path makes no kernel calls.

**PatchGuard.** XP x64 / Server 2003 x64 ship Kernel Patch Protection, which
hashes the kernel's code and bugchecks (`0x109`, CRITICAL_STRUCTURE_CORRUPTION)
on any `SwapContext` patch. To stay safe, WINSIMD installs the hook **only when
PatchGuard is genuinely disabled** — i.e. a kernel debugger is attached
(`KdDebuggerEnabled = 1` and `KdDebuggerNotPresent = 0`). In any normal boot it
detects PatchGuard is active, touches nothing, and reports `GuardReason`
= `CG_PATCHGUARD`, so WINSIMD never bugchecks the machine. (Disabling PatchGuard
outright is tracked separately; the guard itself is verified working with
PatchGuard off — it arms, the hook runs on every switch, and YMM is preserved.)

## Status for applications

* **NT:** the volatile key
  `HKLM\SYSTEM\CurrentControlSet\Services\winsimd\Runtime` holds:
  * `GuardFlags`: 1 = active, 2 = XMM, 4 = YMM, 8 = ZMM;
  * `GuardComponents`: the XCR0-style mask that is saved;
  * `GuardReason`;
  * `Guard`: the full `CG_STATUS` block from `include/ctxguard.h`.

  The key is discarded at shutdown, so it exists only while the 1.2 driver
  is running.
* **9x:** `DeviceIoControl(\\.\WINSIMD, 2)` returns the same `CG_STATUS` block.

An application that needs SSE on NT 3.1–4.0 / 95 should require
`GuardFlags & 3 == 3`. The Flash Player ports (FP10/FP32) do exactly this.

`ContextGuard` (REG_DWORD) in the service key set to 0 disables the guard.
`AvxPolicy=1` still keeps AVX off entirely.

## Install

`WINSIMD.EXE` (NT) / the same EXE on 9x is both the GUI and command-line
installer. The driver file (`WINSIMD.SYS`, or `WINSIMD.VXD` on 9x) must sit next
to it.

| Command | Does |
|---|---|
| `WINSIMD.EXE` | GUI: a Yes/No confirmation, then a progress bar, then a "completed" message. |
| `WINSIMD.EXE /silent` | Install with no UI; prints `completed.` to the console. |
| `WINSIMD.EXE /remove` | Remove WINSIMD if present (GUI, or add `/silent` for console). |

Start type (NT): `/auto` (default), `/system`, `/boot`, `/demand`.
AVX policy: `/avx` (enable, default) or `/safe` (gate AVX). `/start` and
`/stop` control the running service. A reboot loads or unloads the driver.

## Limits

* The amd64 driver guards **YMM only** (YMM0–7 upper halves), not AVX-512
  ZMM/opmask, and only when PatchGuard is disabled — see the XP x64 section
  below. With PatchGuard active (a normal boot) the amd64 guard stays off; for
  several AVX threads on XP x64 without the guard, use `AvxPolicy=1`.
* Multiprocessor support needs a local APIC (MPS/ACPI HALs). Without one the
  guard reports `GuardReason` 2 and stays off.
* At most 32 CPUs are supported.
* Verified in QEMU only:
  * NT 3.1 UP and 2-CPU MP (XMM);
  * NT 4.0 UP (XMM);
  * Windows 2000 UP and 2-CPU MP (YMM);
  * Windows 95 RTM (XMM);
  * AVX under `-cpu max`.

  Not verified:
  * NT 3.5x and Windows 98 first edition;
  * AVX-512 hardware;
  * real multiprocessor machines.

## Tests (`test/`)

| Program | Checks |
|---|---|
| `xmmstrs.c` | 3 threads keep XMM0-6 patterns while spinning (`XMMSTRS [s] [nosound]`) |
| `xmmstr6.c` | the same with 6 threads, to force CPU migrations on MP |
| `ymmstrs.c` | 3 threads keep YMM0-6 (both halves) |

## Build

```sh
bash scripts/build.sh        # i386 + amd64 drivers, installers, WINSINFO
bash scripts/build_vxd.sh    # Windows 9x VxD
E9=1 bash scripts/build.sh   # test build: report on QEMU debug port 0xE9
```

Clang/LLD, NASM, llvm-dlltool, Python 3 and pefile are required.

The NT 3.1 exact-kernel research adapter (`src/nt31_xmm_guard.*`, XMMGUARD,
LOADXMM) is kept for reference only; 1.2 replaces it.

Source lineage: https://github.com/ages2001/WINSIMD, v1.1. LICENSE retained.
