#!/bin/bash
# Build the generic NT 3.x vector context-switch guard (XMMGUARD.SYS) with the
# host LLVM toolchain (clang-18 / lld-link / llvm-dlltool / nasm) -- no mingw
# sysroot needed. The kernel imports are emitted UNDECORATED (DbgPrint,
# MmIsAddressValid, KeBugCheckEx): ntoskrnl exports stdcall routines without the
# @N suffix, so the import NAMES in the driver must match. llvm-dlltool --kill-at
# strips the @N from the import name while keeping the __imp__Name@N link symbol
# the compiler references, so linking resolves AND the kernel loader resolves.
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$PROJ/out/research}"
mkdir -p "$OUT"
cd "$OUT"

CFLAGS="--target=i386-pc-windows-msvc -march=i386 -mno-sse -mno-sse2 -mno-x87 \
 -ffreestanding -fno-stack-protector -fno-builtin -Os -Wall"

# 1. kernel import library with UNDECORATED import names (--kill-at)
cat > ntoskrnl.def <<'EOF'
LIBRARY ntoskrnl.exe
EXPORTS
DbgPrint
MmIsAddressValid@4
KeBugCheckEx@20
EOF
llvm-dlltool -m i386 --kill-at -d ntoskrnl.def -l ntoskrnl.lib

# 2. compile C + assemble asm
clang-18 $CFLAGS -c "$PROJ/src/nt31_xmm_guard.c" -o guard.obj
nasm -f win32 "$PROJ/src/nt31_xmm_guard.asm" -o guard_asm.obj

# 3. link the native kernel driver
lld-link /nologo /driver /subsystem:native,3.10 /osversion:3.10 \
  /entry:DriverEntry@8 /nodefaultlib /safeseh:no /machine:x86 /release \
  /base:0x10000 /out:XMMGUARD.SYS guard.obj guard_asm.obj ntoskrnl.lib

echo "built $OUT/XMMGUARD.SYS ($(stat -c%s XMMGUARD.SYS) bytes)"
