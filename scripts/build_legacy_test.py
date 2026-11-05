"""Build the preemption/migration test with the prepared freestanding toolchain."""
from pathlib import Path
import subprocess
import sys
import os
project=Path(__file__).resolve().parents[1]
root=project.parent
tools=Path(os.environ.get('WINSIMD_TOOLS',root/'tools/usr'))
out=project/'out/test';out.mkdir(parents=True,exist_ok=True)
def run(args):subprocess.run([str(a) for a in args],check=True)
run([tools/'bin/clang-19','--target=i686-w64-windows-gnu','-isystem',tools/'share/mingw-w64/include',
     '-march=i386','-mno-sse','-mno-sse2','-ffreestanding','-fno-stack-protector','-fno-builtin','-Os',
     '-c',project/'test/legacy_xmm.c','-o',out/'legacy_xmm.obj'])
run([tools/'bin/nasm','-f','win32',project/'test/legacy_xmm.asm','-o',out/'legacy_xmm_asm.obj'])
run([tools/'bin/lld-link-19','/nologo','/entry:entry@0','/subsystem:windows,3.10','/osversion:3.10',
     '/nodefaultlib','/safeseh:no','/machine:x86','/out:'+str(out/'XMMTEST.EXE'),
     out/'legacy_xmm.obj',out/'legacy_xmm_asm.obj',tools/'i686-w64-mingw32/lib/libkernel32.a'])
sys.path.insert(0,str(project/'scripts'))
from pad_legacy_pe import pad_image
print(pad_image(out/'XMMTEST.EXE'))
run([tools/'bin/clang-19','--target=i686-w64-windows-gnu','-isystem',tools/'share/mingw-w64/include',
     '-march=i386','-mno-sse','-mno-sse2','-ffreestanding','-fno-stack-protector','-fno-builtin','-Os',
     '-c',project/'test/legacy_churn.c','-o',out/'legacy_churn.obj'])
run([tools/'bin/lld-link-19','/nologo','/entry:entry@0','/subsystem:windows,3.10','/osversion:3.10',
     '/nodefaultlib','/safeseh:no','/machine:x86','/out:'+str(out/'XMMCHURN.EXE'),
     out/'legacy_churn.obj',out/'legacy_xmm_asm.obj',tools/'i686-w64-mingw32/lib/libkernel32.a'])
print(pad_image(out/'XMMCHURN.EXE'))
