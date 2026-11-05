# WINSIMD

A CPU SIMD-enabling driver for **Windows NT 3.1 / 3.5x / 4.0 / 2000 / XP** (i386),
**Windows XP x64** (amd64), and **Windows 95 / 98 / ME**. If the processor supports
them, it makes MMX / SSE..SSE4.2 / AVX / AVX2 (/ AVX-512) usable **without
conflicting with what the OS already does**.

One shared core (`src/core.c` + `src/guard.c` + `src/lowlevel_x86.asm`), three loaders:

| Target | File | Format |
|---|---|---|
| NT 3.1 → XP (x86) | `out/i386/WINSIMD.SYS` | PE kernel driver, native 3.10 |
| XP x64 | `out/amd64/WINSIMD.SYS` | PE32+ kernel driver |
| Windows 9x | `out/9x/WINSIMD.VXD` | LE static VxD |

## What it does

At boot (NT: system-start driver, 9x: `Sys_Critical_Init`), on every logical CPU:

1. **Detect** — read the CPUID feature set and the CPU brand name (brand string, or
   a "vendor family/model/stepping" fallback). It first checks for CPUID via the
   EFLAGS.ID toggle, so a real 386 safely reports "no SIMD" and exits.
2. **Read the OS posture** — CR0 (EM), CR4 (OSFXSR/OSXMMEXCPT/OSXSAVE), XCR0.
3. **Apply the automatic-hybrid policy:**

| Register file | If the OS already preserves it | If the OS does not |
|---|---|---|
| x87 / **MMX** | use it (every OS saves it via FNSAVE/FXSAVE) | — |
| **XMM** (SSE) | leave it — `OS_MANAGED` (2000/XP; 98) | the driver sets CR4.OSFXSR+OSXMMEXCPT — `DRIVER_ENABLED` (NT 3.1/3.5x/4, 95) |
| **YMM** (AVX) | — (no target OS preserves it) | the driver sets CR4.OSXSAVE + XCR0 — `DRIVER_ENABLED` (everywhere) |
| **ZMM** (AVX-512) | — | opmask/ZMM bits are added to XCR0 |

4. **Self-test** — run real SSE/SSE2/AVX instructions inside a guarded block.
5. **Publish** — NT: write to `HKLM\SYSTEM\CurrentControlSet\Services\winsimd\Status`
   (REG_BINARY `ILF_CPU_INFO`) plus friendly DWORDs. 9x: the VxD returns the same
   `ILF_CPU_INFO` via `DeviceIoControl(\\.\WINSIMD, 1)`.

The "no conflict" rule: the driver never touches state the OS preserves; for state
the OS does not preserve (XMM on NT4- and 95, YMM/ZMM everywhere) it enables the
feature, but usage must always happen inside a guarded block (`ilf_simd_begin/end`:
CLI + FXSAVE/XSAVE … FXRSTOR/XRSTOR + restore flags).

## Installation

Put the files for your architecture in one folder (for 9x: `i386\WINSIMD.EXE` +
`i386\WINSINFO.EXE` + `9x\WINSIMD.VXD`), then:

```
WINSIMD                 ; install (auto-start by default) + AvxPolicy=0
WINSIMD /safe           ; install + safe mode (AVX gated, AvxPolicy=1)
WINSIMD /avx            ; install + AVX enabled (AvxPolicy=0, default)
WINSIMD /auto           ; (NT) auto-start   (default)
WINSIMD /system         ; (NT) system-start
WINSIMD /boot           ; (NT) boot-start
WINSIMD /demand         ; (NT) demand-start
WINSIMD start | stop    ; (NT) load/stop without rebooting
WINSIMD remove          ; uninstall
```

The default start type is **auto-start**. On some multiprocessor HALs (seen on
NT 3.51 MPS) a *system*-start driver loads before every application processor is
fully online, so the per-CPU programming could stall and the service was reported
as failed. Auto-start runs once all CPUs are up and still well before any user
program needs SSE/AVX. `/system` and `/boot` remain available if you need an
earlier load.

- **NT:** the driver is copied to `System32\drivers\` and registered as a
  kernel-driver service.
- **9x:** the installer detects 9x itself, copies `WINSIMD.VXD` to `WINDOWS\SYSTEM\`
  and writes the standard static-VxD registration
  (`HKLM\System\CurrentControlSet\Services\VxD\WINSIMD` → `StaticVxD="winsimd.vxd"`,
  `Start=00`). It does not touch SYSTEM.INI. `start`/`stop` are not available on 9x
  (reboot instead).

`WINSIMD.EXE` detects the OS it is running on and picks the right file, printing it:

```
detected OS: Windows NT (5.2)
  -> installing amd64 WINSIMD.SYS (kernel driver, XP x64)
installed service 'winsimd', start=2 (auto)
AVX policy: enabled (default) - AvxPolicy=0
```

Then run **WINSINFO.EXE**: it shows the CPU name and, per feature, whether it is
supported and its state (ENABLED (OS) / ENABLED (driver) / blocked / not present).
On ≤256-colour displays (VGA, NT 3.1, 9x without a driver) it automatically switches
to a pure VGA-16 palette, with no dithering.

## Building (Linux: clang + lld-link + nasm + python3)

```sh
bash scripts/build.sh          # release: out/i386, out/amd64, out/9x
E9=1 bash scripts/build.sh     # + QEMU debug-console (port 0xE9) report
bash scripts/build_test.sh     # concurrency stress test (out/test/SIMDMT64.EXE)
```

- The i386 side is built with a **true 386 baseline**: `-march=i386 -mno-mmx
  -mno-sse… -mno-x87`; the build script verifies via objdump that no `cmov`/SIMD
  leaked into the compiled C. SIMD is used only at run time, after detection,
  through the asm primitives.
- NT drivers are linked with `/release` so a valid PE checksum is written
  (otherwise `STATUS_IMAGE_CHECKSUM_MISMATCH 0xc0000221` on boot drivers). Import
  surface: only symbols present since NT 3.1 (`DbgPrint`, `ZwCreateKey`,
  `ZwSetValueKey`, `ZwOpenKey`, `ZwQueryValueKey`, `ZwClose`,
  `RtlInitUnicodeString`) → one binary loads everywhere.
- **VxD** (`scripts/build_vxd.sh`): the same core + `vxd/vxdmain.c` +
  `vxd/vxdglue.asm` are linked into a PE with lld-link; `scripts/pe2le.py` converts
  the PE to the LE format Win95 loads (base relocations → LE internal fixups,
  DDB → ordinal 1). No VMM services are used, no imports.

## Multi-CPU (SMP) — every core is programmed

CR4/XCR0 are per-logical-CPU; otherwise SSE/AVX faults (#UD) on another core. The
driver migrates the current thread to each CPU in turn (`KeSetSystemAffinityThread`)
and applies the policy there. These symbols are not imported statically — so that
**one i386 binary still loads on NT 3.1** — but are resolved at run time from
ntoskrnl's export table (ntoskrnl is located via the IAT slot of the one import we
do have, `DbgPrint`). When present (NT 3.51 and up expose them) every online CPU is
programmed; when absent (very early NT) only the boot CPU is. Only processors in
`KeActiveProcessors` (the online-CPU mask, also resolved at run time) are targeted,
so the driver never stalls trying to pin to a CPU that is not up yet. 9x is
uniprocessor, so the VxD never uses this path.

Verified (`test/results/win2000_smp_percpu_e9.log`): Windows 2000, QEMU `-smp 4`,
`smp=1 oscpu=2 nproc=2`, and both CPUs read back `cr4=406d0` → each was programmed
independently. (Windows 2000 Pro caps at 2 CPUs; the XP x64 test image uses a
uniprocessor HAL — so the OS activates ≤2 CPUs on the available images. The loop
enumerates `KeNumberProcessors` and scales to 64.)

## Concurrent (multi-threaded) SIMD correctness

`out/test/SIMDMT64.EXE` is a multi-threaded stress test: each thread holds its own
signature in vector registers and counts when another thread's state leaks in across
a context switch (see `test/simdmt_x64.asm`). XP x64, 6 threads
(`test/results/concurrency_xp64.txt`):

| Group | Events | Meaning |
|---|---|---|
| SSE (XMM) | **0** | 2000/XP save the whole FXSAVE area (x87+XMM) on every switch → concurrent SSE is **already safe** |
| AVX (YMM) | **5574** | no NT/9x scheduler saves the upper 128 bits of YMM → concurrent AVX threads corrupt each other |

This is independent of CPU count: it reproduces even on a single CPU, because threads
are time-sliced and context-switch between each other.

**Status:** concurrent **SSE/SSE2/.../SSE4.2** is already safe on 2000/XP. Making
concurrent **AVX/AVX-512** transparently safe (without changing the application)
requires preserving the YMM/ZMM upper state per thread across context switches —
i.e. hooking into the OS thread-switch path. That is a separate, careful
OS-integration step and is **not in this release**. `SIMDMT64.EXE` is its pass/fail
oracle (target: AVX events = 0).

### SSE on multi-core NT 3.1 / 3.5x / 4.0 (important)

The same mechanism has a consequence that shows up even for an ordinary,
single-threaded SSE program on these OSes:

- **2000/XP** manage the XMM register file themselves (they set CR4.OSFXSR and
  save/restore all of XMM on every context switch). There SSE is safe on any number
  of CPUs — the driver only adds AVX there.
- **NT 3.1 / 3.5x / 4.0** (and 9x) predate OS SSE support: their scheduler saves only
  x87/MMX, **never XMM**. WINSIMD sets CR4.OSFXSR so SSE instructions don't #UD, but
  the OS still does not preserve XMM. On a **single CPU** a program that is the only
  XMM user keeps its registers across switches (nothing else overwrites them), so it
  works. On **multiple CPUs** the scheduler can migrate the thread to another core,
  and the physical XMM registers do **not** travel with it — after a migration the
  thread sees the other core's stale XMM. The result: an SSE program on multi-core
  pre-NT4 runs without #UD but produces wrong results (this is exactly issue #1 on
  NT 3.51: "works only if one CPU core is active"). It is the same class of problem
  as the AVX case above, for XMM instead of YMM.

  Until per-thread vector-state preservation lands, the options on pre-NT4
  multiprocessor systems are:
  - **Run the SSE program on one CPU.** System-wide:
    `HKLM\System\CurrentControlSet\Control\Session Manager\RegisteredProcessors = 1`
    (the workaround confirmed in issue #1), or boot a uniprocessor HAL. Per-process
    CPU affinity (e.g. `start /affinity 1 prog.exe` on NT4+) also avoids migration.
  - Use the SSE build path only where the OS manages XMM (2000/XP), where it is safe
    regardless of core count.

  WINSIMD still programs CR4 on every core (so there is no #UD), but it cannot make
  the XMM *content* follow a migrating thread without the context-switch integration
  noted above.

### Safe mode (AvxPolicy) — a controlled choice instead of silent corruption

For this gap, the driver has a **registry-controlled policy**:

- `AvxPolicy=0` (**default**): AVX is enabled. Single-threaded AVX and concurrent SSE
  are safe; concurrent AVX **can corrupt** between threads.
- `AvxPolicy=1` (**safe mode**): the driver does **not** enable AVX (it leaves
  CR4.OSXSAVE/XCR0 untouched). Applications see AVX as unusable via CPUID and **fall
  back to SSE**, which is preserved, so concurrent use is safe. SSE stays enabled.

`HKLM\SYSTEM\CurrentControlSet\Services\winsimd\AvxPolicy` (REG_DWORD). The installer
writes 1 with `WINSIMD /safe`, 0 otherwise. WINSINFO shows "SAFE MODE" in the header
and marks the AVX rows "off (safe mode)".

Verification (`SIMDMT64.EXE`, 6 threads; `test/results/safemode_xp64.txt`):

| OS | AvxPolicy | Driver (E9) | Test result |
|---|---|---|---|
| XP x64 | 0 (default) | `safe=0 cr4 6f8→406f8` avx=driver | AVX **5574 corruptions** |
| XP x64 | 1 (safe) | `safe=1 cr4 6f8→6f8` avx=blocked | AVX→SSE fallback, **0 corruptions** |
| Win2000 x86 | 1 (safe) | `safe=1 cr4 6d0→6d0` (cpu0+cpu1) avx=blocked | — |

## Test status (QEMU 8.2.2, TCG)

| Target | Storage / CPU | Result |
|---|---|---|
| Windows 2000 | AHCI, Haswell | ✅ cr4 6d0→406d0, sse=OS, avx=driver, self-test SSE2+AVX OK, GUI ✅ |
| Windows XP x64 | IDE, Haswell | ✅ cr4 6f8→406f8, sse=OS, avx=driver, self-test SSE2+AVX OK |
| Windows NT 3.1 | IDE, Haswell | ✅ cr4 0→40600, sse=driver, avx=driver, self-test SSE2+AVX OK |
| Windows 95 | IDE, Haswell | ✅ LoadSuccess → SYSCRITINIT/DEVICEINIT/INITCOMPLETE SUCCESS; cr4 0→40600, sse=driver, avx=driver, self-test SSE2+AVX OK, GUI ✅ |
| Windows 95 | IDE, Pentium III | ✅ cr4 0→600, sse=driver, self-test SSE OK |

The headless method: the VHD is edited offline (mtools for FAT, hivexsh for the NT
registry), an E9-build driver writes a boot summary to **port 0xE9**, captured via
QEMU `-debugcon` (no kernel debugger needed).

## Files

```
include/ilf.h            shared types, ILF_CPU_INFO, IOCTL codes
src/core.c               CPUID detection + CPU name + automatic-hybrid policy
src/smp.c                programs every logical CPU (resolves SMP symbols at run time)
src/ntosprobe_x86/x64.asm locates ntoskrnl via the DbgPrint IAT slot (driver only)
src/guard.c              simd_begin/end + self-test (FXSAVE/XSAVE area aligned at run time)
src/lowlevel_x86/x64.asm ring-0 primitives + self-test kernels + E9
src/regwrite.c           NT: writes result to the registry; reads AvxPolicy
src/intlfxsr.c           NT: DriverEntry
vxd/vxdmain.c            9x: Sys_Critical_Init + W32_DEVICEIOCONTROL (C)
vxd/vxdglue.asm          9x: DDB + register-ABI control procedure
installer/ilfinst.c      WINSIMD.EXE — OS-detecting installer (NT SCM / 9x StaticVxD), CRT-free
gui/wininfo.c            WINSINFO.EXE — CRT-free Win32 GUI (NT: registry, 9x: DeviceIoControl)
scripts/build.sh         full build (NT x86/x64 + 9x)
scripts/build_vxd.sh     9x VxD build
scripts/build_test.sh    builds the concurrency stress test
scripts/pe2le.py         PE → LE (VxD) converter
scripts/lewrite.py       generic LE writer (+ byte-exact "repack" test of a stock VxD)
scripts/ledump.py        LE/VxD dumper (header, objects, fixups, DDB)
scripts/nt31_inject.py   NT 3.1 v1.1 hive service injector (see its header)
test/simdmt.c            multi-threaded SIMD concurrency stress test (SSE/AVX corruption counter)
test/simdmt_x64.asm      the stress test's vector-register probes
test/results/            logs, screenshots, and findings from the runs above
out/                     prebuilt binaries (i386, amd64, 9x, test)
```

## Open notes

- **Concurrent AVX/AVX-512 (transparent):** not in this release — see "Concurrent
  SIMD correctness" above. Safe mode (`AvxPolicy=1`) is the shipped mitigation.
- **SSE on multi-core NT 3.1/3.5x/4.0:** XMM is not preserved across CPU migration
  on these OSes — see "SSE on multi-core NT 3.1/3.5x/4.0" above. Run such programs
  on one CPU, or use 2000/XP where the OS manages XMM. Tracked as issue #1.
- **Windows 98/ME:** the same VxD; if 98 sets OSFXSR itself the policy reports SSE as
  `OS_MANAGED`. Not tested here with a 98/ME image.
- **NT 3.1 hive:** `regf` v1.1; `nt31_inject.py` produces a valid hive but NT 3.1's
  boot-time auto-start did not pick up the injected key, so on real NT 3.1 install
  with `WINSIMD.EXE`.

## License

Public domain / do as you wish. No warranty — this programs control registers and
loads as a boot driver; test in a VM first.
