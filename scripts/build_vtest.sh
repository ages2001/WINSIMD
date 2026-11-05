#!/bin/bash
# Build VTEST.EXE, the vector-guard verifier for NT 3.1+ (x86), with the host
# LLVM toolchain (clang-18 / lld-link / llvm-dlltool / nasm). CRT-free Win32 GUI
# subsystem (no console needed; results go to C:\FP32\RESULT.LOG). Imports from
# kernel32/advapi32 are emitted undecorated via llvm-dlltool --kill-at, so the
# NT 3.1 loader resolves them against the real stdcall exports.
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$PROJ/out/research}"
mkdir -p "$OUT"
cd "$OUT"

CFLAGS="--target=i386-pc-windows-msvc -march=i386 -mno-sse -mno-sse2 -mno-x87 \
 -ffreestanding -fno-stack-protector -fno-builtin -Os -Wall"

cat > kernel32.def <<'EOF'
LIBRARY kernel32.dll
EXPORTS
CreateFileA
WriteFile
FlushFileBuffers
CloseHandle
GetWindowsDirectoryA
CopyFileA
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
GetLastError
GetExitCodeThread
ExitProcess
EOF
cat > advapi32.def <<'EOF'
LIBRARY advapi32.dll
EXPORTS
OpenSCManagerA
CreateServiceA
OpenServiceA
StartServiceA
QueryServiceStatus
CloseServiceHandle
EOF
llvm-dlltool -m i386 --kill-at -d kernel32.def -l kernel32.lib
llvm-dlltool -m i386 --kill-at -d advapi32.def -l advapi32.lib

clang-18 $CFLAGS -c "$PROJ/test/vtest.c" -o vtest.obj
nasm -f win32 "$PROJ/test/vtest_probe.asm" -o vtest_probe.obj

lld-link /nologo /entry:vtest_start /subsystem:windows,3.10 /osversion:3.10 \
  /nodefaultlib /safeseh:no /machine:x86 /release \
  /out:VTEST.EXE vtest.obj vtest_probe.obj kernel32.lib advapi32.lib

# Materialize zero-filled section tails so the NT 3.1 user-mode loader maps the
# image cleanly (it does not synthesize the virtual-size tail the way NT 4.0+ do).
python3 "$PROJ/scripts/pad_legacy_pe.py" VTEST.EXE >/dev/null

echo "built $OUT/VTEST.EXE ($(stat -c%s VTEST.EXE) bytes)"
