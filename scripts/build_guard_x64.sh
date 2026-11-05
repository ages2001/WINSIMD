#!/bin/bash
# Build the XP x64 / Server 2003 x64 YMM context-switch guard (YMMGUARD.SYS, amd64).
# Host LLVM toolchain. x64 kernel exports are undecorated, so no --kill-at.
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$PROJ/out/amd64}"
mkdir -p "$OUT"; cd "$OUT"

CFLAGS="--target=x86_64-pc-windows-msvc -mno-sse -mno-sse2 -ffreestanding \
 -fno-stack-protector -fno-builtin -Os -Wall"
# -mno-sse/sse2 keeps the C from emitting vector ops; the guard's own AVX is all
# in the .asm. (clang still allows the build; the C touches no floats.)

cat > ntoskrnl64.def <<'EOF'
LIBRARY ntoskrnl.exe
EXPORTS
DbgPrint
MmIsAddressValid
KeBugCheckEx
ExAllocatePoolWithTag
EOF
llvm-dlltool -m i386:x86-64 -d ntoskrnl64.def -l ntoskrnl64.lib

clang-18 $CFLAGS -c "$PROJ/src/nt64_ymm_guard.c" -o guard64.obj
nasm -f win64 "$PROJ/src/nt64_ymm_guard.asm" -o guard64_asm.obj

lld-link /nologo /driver /subsystem:native,5.02 /osversion:5.02 \
  /entry:DriverEntry /nodefaultlib /machine:x64 /release \
  /out:YMMGUARD.SYS guard64.obj guard64_asm.obj ntoskrnl64.lib

echo "built $OUT/YMMGUARD.SYS ($(stat -c%s YMMGUARD.SYS) bytes)"
