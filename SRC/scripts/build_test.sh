#!/usr/bin/env bash
# build_test.sh - multi-threaded SIMD concurrency stress test (XP x64 + x86).
set -euo pipefail
cd "$(dirname "$0")/.."
B=build/test; mkdir -p $B out/test
for dll in kernel32 user32; do
  llvm-dlltool -m i386        --kill-at -d scripts/$dll.def -D $dll.dll -l "$B/${dll}_x86.lib"
  llvm-dlltool -m i386:x86-64           -d scripts/$dll.def -D $dll.dll -l "$B/${dll}_x64.lib"
done
nasm -f win64 test/simdmt_x64.asm -o $B/simdmt_x64.obj
clang --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -fno-builtin \
      -Os -Wall -c test/simdmt.c -o $B/simdmt_c_x64.obj
lld-link /nologo /subsystem:windows,5.02 /entry:simdmt_start /nodefaultlib /machine:x64 \
  /out:out/test/SIMDMT64.EXE $B/simdmt_c_x64.obj $B/simdmt_x64.obj \
  "$B/kernel32_x64.lib" "$B/user32_x64.lib"
echo "built out/test/SIMDMT64.EXE"
