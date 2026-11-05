#!/bin/bash
# Build WTEST.EXE, the native-behavior probe: the same 4-worker XMM/YMM migration
# workload as VTEST but WITHOUT installing the guard, so it measures what the OS
# preserves by itself. Host LLVM toolchain; undecorated imports via --kill-at.
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$PROJ/out/research}"
mkdir -p "$OUT"; cd "$OUT"
CFLAGS="--target=i386-pc-windows-msvc -march=i386 -mno-sse -mno-sse2 -mno-x87 \
 -ffreestanding -fno-stack-protector -fno-builtin -Os -Wall"
cat > kernel32_w.def <<'EOF'
LIBRARY kernel32.dll
EXPORTS
CreateFileA
WriteFile
FlushFileBuffers
CloseHandle
CreateThread
WaitForMultipleObjects
WaitForSingleObject
CreateEventA
SetEvent
GetTickCount
GetSystemInfo
Sleep
GetProcAddress
GetModuleHandleA
GetExitCodeThread
ExitProcess
EOF
llvm-dlltool -m i386 --kill-at -d kernel32_w.def -l kernel32_w.lib
clang-18 $CFLAGS -c "$PROJ/test/wtest.c" -o wtest.obj
nasm -f win32 "$PROJ/test/vtest_probe.asm" -o wtest_probe.obj
lld-link /nologo /entry:wtest_start /subsystem:windows,3.10 /osversion:3.10 \
  /nodefaultlib /safeseh:no /machine:x86 /release \
  /out:WTEST.EXE wtest.obj wtest_probe.obj kernel32_w.lib
python3 "$PROJ/scripts/pad_legacy_pe.py" WTEST.EXE >/dev/null
echo "built $OUT/WTEST.EXE ($(stat -c%s WTEST.EXE) bytes)"
