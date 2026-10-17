#!/usr/bin/env bash
# build_vxd.sh - build the Windows 9x VxD (out/9x/WINSIMD.VXD).
# The same core.c / guard.c / lowlevel_x86.asm as the NT driver (true-386
# baseline) + vxd/vxdmain.c + vxd/vxdglue.asm are linked into a PE with
# lld-link, and scripts/pe2le.py converts that PE into the LE Win95 loads.
# E9=1 adds the QEMU port-0xE9 report (test builds only).
set -euo pipefail
cd "$(dirname "$0")/.."
B=build/vxd; mkdir -p $B out/9x
E9FLAG=""; if [ "${E9:-0}" = "1" ]; then E9FLAG="-DILF_E9_DEBUG"; else E9=; fi
CF="--target=i386-pc-windows-msvc -march=i386 -mno-mmx -mno-sse -mno-sse2 -mno-sse3 \
    -mno-avx -mno-x87 -ffreestanding -fno-stack-protector -fno-builtin -Os -Wall -Iinclude $E9FLAG"
nasm -f win32 src/lowlevel_x86.asm -o $B/ll.obj
nasm -f win32 ${E9:+-DE9TRACE} vxd/vxdglue.asm -o $B/glue.obj
for f in src/core.c src/guard.c vxd/vxdmain.c; do
  o=$B/$(basename $f .c).obj
  clang $CF -c $f -o $o
  n=$(llvm-objdump -d $o | grep -ciE '\b(cmov|movss|movsd|xmm|ymm)\b' || true)
  [ "$n" = "0" ] || { echo "!! i386 baseline violation in $f"; exit 1; }
done
# one merged, writable section; keep everything (no /opt:ref): nothing
# references the DDB from code, VMM finds it through the LE entry table.
lld-link /nologo /dll /noentry /nodefaultlib /machine:x86 /base:0x10000000 \
  /opt:noref /merge:.rdata=.text /merge:.data=.text /merge:.bss=.text \
  /section:.text,RWE /out:$B/winsimd_vxd.dll \
  $B/glue.obj $B/ll.obj $B/core.obj $B/guard.obj $B/vxdmain.obj
python3 -I scripts/pe2le.py $B/winsimd_vxd.dll out/9x/WINSIMD.VXD
