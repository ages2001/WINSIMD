#!/usr/bin/env bash
# build.sh - build the WINSIMD family with clang/lld-link/nasm.
#   out/i386/WINSIMD.SYS    kernel driver (NT 3.1 -> XP x86), true-386 baseline
#   out/amd64/WINSIMD.SYS   kernel driver (XP x64)
#   out/<arch>/WINSIMD.EXE  console installer
#   out/<arch>/WINSINFO.EXE GUI info tool (NT: driver's Status reg key;
#                           9x: DeviceIoControl to the VxD)
#   out/9x/WINSIMD.VXD      Windows 9x VxD (same core, via scripts/build_vxd.sh)
# E9=1 also compiles the port-0xE9 verification path into the driver.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd); B="$ROOT/build"; OUT="$ROOT/out"
rm -rf "$B" "$OUT"; mkdir -p "$B" "$OUT/i386" "$OUT/amd64"

CC=clang
E9FLAG=""; [ "${E9:-0}" = "1" ] && E9FLAG="-DILF_E9_DEBUG"
CF_I386="--target=i386-pc-windows-msvc -march=i386 -mno-mmx -mno-sse -mno-sse2 \
         -mno-sse3 -mno-avx -mno-x87 -ffreestanding -fno-stack-protector \
         -fno-builtin -Os -Wall -Iinclude $E9FLAG"
CF_X64="--target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector \
        -fno-builtin -mno-sse -mno-sse2 -Os -Wall -Iinclude $E9FLAG"
SUBSYS_I386="native,3.10"; SUBSYS_X64="native,5.02"

echo "== import libraries =="
# ntoskrnl: --kill-at so stdcall asm-labels (_Name) resolve; DbgPrint (cdecl) too.
llvm-dlltool -m i386        --kill-at -d scripts/ntoskrnl.def -D ntoskrnl.exe -l "$B/ntoskrnl_x86.lib"
llvm-dlltool -m i386:x86-64           -d scripts/ntoskrnl.def -D ntoskrnl.exe -l "$B/ntoskrnl_x64.lib"
for dll in kernel32 advapi32 user32 gdi32; do
  llvm-dlltool -m i386        --kill-at -d scripts/$dll.def -D $dll.dll -l "$B/${dll}_x86.lib"
  llvm-dlltool -m i386:x86-64           -d scripts/$dll.def -D $dll.dll -l "$B/${dll}_x64.lib"
done

echo "== assemble primitives =="
nasm -f win32 src/lowlevel_x86.asm -o "$B/ll_x86.obj"
nasm -f win64 src/lowlevel_x64.asm -o "$B/ll_x64.obj"
nasm -f win32 src/ntosprobe_x86.asm -o "$B/probe_x86.obj"
nasm -f win64 src/ntosprobe_x64.asm -o "$B/probe_x64.obj"

echo "== compile driver C =="
for f in core guard regwrite smp intlfxsr; do
  $CC $CF_I386 -c "src/$f.c" -o "$B/${f}_x86.obj"
  $CC $CF_X64  -c "src/$f.c" -o "$B/${f}_x64.obj"
done
# i386 baseline check (no cmov/SIMD leaked into C)
for f in core guard regwrite smp intlfxsr; do
  n=$(llvm-objdump -d "$B/${f}_x86.obj" | grep -ciE '\b(cmov|movss|movsd|xmm|ymm)\b' || true)
  [ "$n" = "0" ] || { echo "!! i386 baseline violation in $f ($n)"; exit 1; }
done
echo "   i386 baseline clean"

echo "== link drivers =="
lld-link /nologo /driver /subsystem:$SUBSYS_I386 /entry:DriverEntry@8 /nodefaultlib \
  /release /machine:x86 /base:0x10000 /out:"$OUT/i386/WINSIMD.SYS" \
  "$B/ll_x86.obj" "$B/core_x86.obj" "$B/guard_x86.obj" "$B/regwrite_x86.obj" \
  "$B/smp_x86.obj" "$B/probe_x86.obj" "$B/intlfxsr_x86.obj" "$B/ntoskrnl_x86.lib"
lld-link /nologo /driver /subsystem:$SUBSYS_X64 /entry:DriverEntry /nodefaultlib \
  /release /machine:x64 /base:0x10000 /out:"$OUT/amd64/WINSIMD.SYS" \
  "$B/ll_x64.obj" "$B/core_x64.obj" "$B/guard_x64.obj" "$B/regwrite_x64.obj" \
  "$B/smp_x64.obj" "$B/probe_x64.obj" "$B/intlfxsr_x64.obj" "$B/ntoskrnl_x64.lib"

echo "== installer =="
$CC $CF_I386 -c installer/ilfinst.c -o "$B/inst_x86.obj"
$CC $CF_X64  -c installer/ilfinst.c -o "$B/inst_x64.obj"
lld-link /nologo /subsystem:console,3.10 /entry:ilf_start@0 /nodefaultlib /machine:x86 \
  /out:"$OUT/i386/WINSIMD.EXE" "$B/inst_x86.obj" "$B/kernel32_x86.lib" "$B/advapi32_x86.lib"
lld-link /nologo /subsystem:console,5.02 /entry:ilf_start /nodefaultlib /machine:x64 \
  /out:"$OUT/amd64/WINSIMD.EXE" "$B/inst_x64.obj" "$B/kernel32_x64.lib" "$B/advapi32_x64.lib"

echo "== WINSINFO GUI =="
$CC $CF_I386 -c gui/wininfo.c -o "$B/gui_x86.obj"
$CC $CF_X64  -c gui/wininfo.c -o "$B/gui_x64.obj"
lld-link /nologo /subsystem:windows,3.10 /entry:wininfo_start@0 /nodefaultlib /machine:x86 \
  /out:"$OUT/i386/WINSINFO.EXE" "$B/gui_x86.obj" \
  "$B/kernel32_x86.lib" "$B/user32_x86.lib" "$B/gdi32_x86.lib" "$B/advapi32_x86.lib"
lld-link /nologo /subsystem:windows,5.02 /entry:wininfo_start /nodefaultlib /machine:x64 \
  /out:"$OUT/amd64/WINSINFO.EXE" "$B/gui_x64.obj" \
  "$B/kernel32_x64.lib" "$B/user32_x64.lib" "$B/gdi32_x64.lib" "$B/advapi32_x64.lib"

echo "== Windows 9x VxD =="
E9="${E9:-0}" bash scripts/build_vxd.sh 2>&1 | grep -v "already merged"

echo "== done =="
ls -l "$OUT/i386" "$OUT/amd64" "$OUT/9x"
file "$OUT"/i386/* "$OUT"/amd64/* "$OUT"/9x/*
