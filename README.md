# WINSIMD 1.2 — generic vector context-switch guard (XMM / YMM)

WINSIMD 1.1 enables the CPU's SIMD state bits (CR4 / XCR0) on every logical
processor so SSE/AVX instructions stop faulting on old Windows. That is
necessary but **not sufficient** for *concurrent* SIMD: old Windows schedulers
do not save and restore the full vector register file across a thread switch, so
two threads that both use those registers corrupt each other when they are
preempted or migrate between CPUs. NT 3.1/3.5x/4.0 save no SIMD state at all;
NT 5.0 (Windows 2000) saves XMM but not the AVX (YMM) upper halves.

WINSIMD 1.2 adds a research adapter — `XMMGUARD.SYS` — that supplies exactly the
missing piece: **per-thread save/restore of the vector register state the OS
does not keep, at the kernel's own context-switch site**. It is loaded in
addition to the WINSIMD 1.1 driver, which still does the CR4/XCR0 enabling.

## What changed since 1.1

- **Transparent per-thread XMM/YMM preservation** across preemption and CPU
  migration, with the width chosen from the CPU (XMM on SSE2, YMM on AVX).
- **Generic kernel discovery.** The guard no longer hard-codes a kernel
  timestamp or image base. At load it locates the running kernel, scans its
  image for the unique context-switch instruction bytes of a known layout, and
  only hooks when they occur **exactly once**; on any kernel it cannot positively
  identify it refuses cleanly (no patch, no crash). Adding an OS = adding one
  validated row. Two rows ship: NT 3.1 (UP + MP) and NT 5.0 / Windows 2000
  (UP + MP).
- **Per-CPU outgoing-thread tracking.** The guard records the thread each CPU
  last switched to, so only the *incoming*-thread register has to be readable at
  the hook site. This is what makes NT 5.0 hookable, where the outgoing-thread
  register has already been reused by the time of the context-switch count.
- **Cooperative (upper-only) mode.** On a kernel that already preserves the low
  128 bits + MXCSR itself (NT 5.0), the guard keeps *only* the YMM upper halves
  (vextractf128 / vinsertf128), leaving the OS's XMM state untouched so it is not
  disturbed. On a kernel that preserves nothing (NT 3.1) it owns the whole
  register.
- **Host-only toolchain.** The adapter and its verifier build with stock
  clang-18 / lld-link / llvm-dlltool / nasm — no MinGW sysroot required.

## Supported kernels and register-state coverage

| Kernel | Mode | What the guard preserves | Verified |
|---|---|---|---|
| NT 3.1 UP + MP (x86) | full | XMM0–7 + MXCSR, and YMM0–7 upper on AVX | 4 CPUs, 4 workers, forced migration — **0 mismatches** |
| Windows 2000 UP + MP (x86, NT 5.0) | upper-only | YMM0–7 upper halves (OS keeps XMM/MXCSR) | 2 CPUs, 4 workers, forced migration — YMM **895→0**, XMM stays 0 |
| Windows XP UP + PAE (x86, NT 5.1) | upper-only | YMM0–7 upper halves (OS keeps XMM/MXCSR) | 2 CPUs, 4 workers, forced migration — YMM **0**, XMM 0 |

| State | NT 3.1 | Windows 2000 |
|---|---|---|
| XMM0–7 + MXCSR | guard preserves | OS preserves natively (guard does not touch) |
| YMM0–7 upper 128 | guard preserves (AVX) | **guard preserves** (AVX) |
| x87 / MMX | native NT scheduler | native NT scheduler |
| ZMM0–7 + opmask | code path present, **unverified** (no AVX-512 host) | n/a |

Enabling CR4/XCR0 is not the same as preserving registers across thread
switches. Pinning a process to one CPU does not prevent corruption between
competing SIMD threads on an OS that does not preserve their state; this guard
addresses that corruption directly.

## Experimental / in progress

- **Windows XP x64 / Server 2003 x64 (`YMMGUARD.SYS`, amd64).** A 64-bit build of
  the same upper-only YMM approach (`src/nt64_ymm_guard.{c,asm}`,
  `scripts/build_guard_x64.sh`; reverse-engineering in `src/NOTES-xp-x64.md`).
  The driver loads, finds the kernel, scans the real SwapContext site uniquely,
  and installs a 14-byte absolute-jump inline hook with a pool trampoline.
  **PatchGuard is neutralized by booting `boot.ini /debug`** (no 0x109). The
  reboot loop was fixed (round-robin slot eviction + limiting the vector ops to
  YMM0–7); install, PatchGuard-bypass and stability are verified. YMM0–7 upper
  halves are preserved (YMM8–15 are not — see the notes).
- **Windows 9x thread-switch guard (`WINSIMD.VXD`).** Win9x has no NT scheduler,
  so the VxD hooks the VMM scheduler via `Call_When_Thread_Switched` and does
  per-thread save/restore of the vector state the OS does not keep
  (`vxd/vxdsched.asm`, `vxd/vxdmain.c`). Mode is chosen at load from
  `CR4.OSFXSR`, like the NT guard: if VMCPD is **not** SSE-aware (Win95 RTM /
  Win98 FE) the guard owns the **full** XMM0–7 (or YMM0–7) + MXCSR; if it **is**
  SSE-aware (Win98 SE / Me) the OS keeps XMM/MXCSR itself, so the guard keeps
  **only** the YMM upper halves (upper-only), and with no AVX present it does
  nothing. Verified in QEMU: on **Windows 95 RTM** (full mode) the 4-thread
  `W9XTEST` concurrency test drops vector corruption from 348/282 (enable-only
  baseline) to 81/75 with the guard, with the system fully stable; on
  **Windows Me** (SSE-aware, no AVX) the guard correctly defers to VMCPD and the
  test is **0 mismatches (PASS)**. The residual on Win95 is imperfect
  incoming-thread identification when 3+ threads interleave. See
  `test/results/win9x-guard/`.

## How it works

1. **Locate the kernel.** Walk down from a known import (`DbgPrint`) to the
   kernel's `MZ`/`PE` base; read its `SizeOfImage`.
2. **Find the hook site generically.** Scan the kernel image for the exact bytes
   of the per-thread context-switch-count increment
   (`inc dword [reg+KTHREAD.ContextSwitches]`, plus any following bytes needed to
   reach a 5-byte displacement). Those bytes encode the KTHREAD offset and the
   register holding the incoming thread, so they are unique to one KTHREAD
   layout. Exactly one match → hook site; zero or several → refuse. (Adding
   another OS = adding one validated row: its bytes, incoming-thread register,
   KTHREAD.State offset, and mode.)
3. **Install atomically.** Replace the located site with a 5-byte `jmp` to the
   hook via a single aligned `lock cmpxchg8b`, so no CPU ever observes a torn
   instruction. The displaced instruction(s) are copied into a generated
   trampoline and re-executed there, so their register/offset need not be
   hard-coded.
4. **Preserve state on every switch.** The hook reads only the *incoming* thread
   from its pushad-frame slot; the outgoing thread is the one this CPU last
   switched to, tracked per-CPU by the guard (so kernels that have already reused
   the outgoing-thread register are still hookable). It enables the per-CPU state
   bits (CR4 OSFXSR/OSXMMEXCPT, and OSXSAVE + `xsetbv` XCR0 for AVX) once per CPU,
   then saves the outgoing thread's vector state and restores the incoming
   thread's, keyed by KTHREAD pointer in a bounded 512-entry table. A terminated
   thread's slot is reclaimed (KTHREAD.State == Terminated).

The width (XMM / YMM / ZMM) and XCR0 are detected once at load. In **full** mode
save/restore is `movups` (XMM), `vmovups ymm` (YMM) or `vmovups zmm` + `kmovq`
(ZMM); in **upper-only** mode (a kernel that keeps the low 128 bits itself) it is
`vextractf128` / `vinsertf128` on the YMM upper halves alone.

## Verifying

Use a disposable VM copy. The four-CPU image was prepared with the supplied
NT 3.1 MP kernel/HAL (`ntkrnlmp` + MP HAL) and the WINSIMD 1.1 driver. QEMU:
`-cpu Haswell -smp 4 -m 128 -machine pc -accel tcg,thread=multi -global
apic.vapic=false`, classic VGA. No OS images are included.

Copy `out/research/XMMGUARD.SYS` and `out/research/VTEST.EXE` into `C:\FP32`
and run `VTEST.EXE`. It installs and starts the guard service, then runs an
XMM phase and (on AVX hardware) a YMM phase: four workers each hold the vector
register file across real preemption while being migrated across every CPU, and
any state that leaked across a context switch is counted. Results are written to
`C:\FP32\RESULT.LOG`; the guard also reports its install line on QEMU debug port
0xE9. `WTEST.EXE` runs the same workload *without* installing the guard, to show
what the OS preserves on its own.

Verified runs are in `test/results/`:

NT 3.1 MP (4 CPUs, full mode):
- `nt31-mp-4cpu-guard-install-e9.txt` — `XMMGUARD width=1 mode=0 site=0x604b0
  cpus=4` (unique site found by scan on the MP kernel, all 4 CPUs observed).
- `nt31-mp-4cpu-xmm-ymm-migration.txt` — 4 workers, hundreds of XMM + YMM
  affinity-migration rounds, **0 XMM / 0 YMM / 0 MXCSR / 0 x87 mismatches**.

Windows 2000 (2 CPUs, upper-only mode):
- `win2000-native-no-guard.txt` — without the guard: XMM 0, **YMM 895** leaks
  (the OS keeps XMM but not the AVX upper halves).
- `win2000-guard-install-e9.txt` — `XMMGUARD width=1 mode=1 site=0x66df3 cpus=2`.
- `win2000-guard-ymm-migration.txt` — with the guard: **YMM 895 → 0**, XMM still
  0 (the guard fixes AVX concurrency without disturbing the OS's XMM state).

## Build

Requires clang-18, lld-link, llvm-dlltool, nasm, Python 3 and `pefile`
(`pip install pefile`). No MinGW sysroot is needed.

```sh
scripts/build_guard_generic.sh out/research   # XMMGUARD.SYS (the guard driver)
scripts/build_vtest.sh         out/research   # VTEST.EXE   (the verifier)
```

The original general WINSIMD driver build is still `scripts/build.sh`; it does
not include this research hook. `scripts/build_nt31_guard.py` remains for the
original exact-kernel MinGW build and is superseded by
`build_guard_generic.sh`.

## Installing

`scripts/build.sh` also builds the CRT-free console installer
(`out/<arch>/WINSIMD.EXE`) and the `WINSINFO.EXE` tool (both carrying an
embedded app icon). `WINSIMD.EXE install` auto-detects the OS: on NT it
registers the kernel-driver service; on Win9x it installs `WINSIMD.VXD` as a
static VxD. On every OS it then stages the tool into `%SystemRoot%\WINSIMD` and
creates launcher + uninstall entry points chosen by the shell (`installer/`):

- **Program Manager shells (NT 3.1/3.5x)** — a `WINSIMD` program group via DDE
  with a *WINSIMD Info* tool item and an *Uninstall WINSIMD* item carrying a
  generated red-X icon (these shells have no Add/Remove Programs).
- **Explorer shells (Win9x / NT 4.0+ / 2000 / XP)** — a Start-Menu `.lnk` for
  the tool via IShellLink, and the uninstall registered in Add/Remove Programs.

`ole32` is bound at runtime (NT 3.1 has no `ole32.dll`) and the Start-Menu path
comes from the registry, so the one `WINSIMD.EXE` loads and runs from NT 3.1
through XP. `WINSIMD.EXE remove` reverses everything. Verified on NT 3.1
(Program Manager) and Windows 95 (Start Menu + Add/Remove Programs).

## Limits

- **x86 only.** x64 saves vector state eagerly in its own context frame, and
  Windows 9x uses a VxD; each needs its own adapter.
- **ZMM / AVX-512 is unverified.** The width-2 path exists but was never run on
  AVX-512 hardware; do not treat it as validated.
- **Demand-load research driver, no unload path.** Reboot before replacing it.
- The 512-entry table is bounded on purpose: exhaustion deliberately bugchecks
  (`0xE0120001`) rather than silently substituting state. This does not prove an
  unlimited simultaneous-thread capacity.
- This is a research adapter for enabling concurrent SIMD on legacy NT, not a
  certified universal SIMD driver or a finished application port.

Source lineage: https://github.com/ages2001/WINSIMD. The XMM context-switch
mechanism was contributed for SSE2 NT 3.1; WINSIMD 1.2 generalizes it to any
NT kernel sharing the layout and extends it to YMM. The supplied LICENSE is
retained.
