"""Build the exact-kernel NT 3.1 UP/MP research adapter, not a 1.2 release."""
from pathlib import Path
import subprocess
import sys
import os
project=Path(__file__).resolve().parents[1];root=project.parent;tools=Path(os.environ.get('WINSIMD_TOOLS',root/'tools/usr'))
out=project/'out/research';out.mkdir(parents=True,exist_ok=True)
def run(args):subprocess.run([str(x) for x in args],check=True)
def compile_c(source,obj):run([tools/'bin/clang-19','--target=i686-w64-windows-gnu','-isystem',tools/'share/mingw-w64/include','-march=i386','-mno-sse','-mno-sse2','-mno-x87','-ffreestanding','-fno-stack-protector','-fno-builtin','-Os','-c',source,'-o',obj])
compile_c(project/'src/nt31_xmm_guard.c',out/'guard.obj')
run([tools/'bin/nasm','-f','win32',project/'src/nt31_xmm_guard.asm','-o',out/'guard_asm.obj'])
run([tools/'bin/lld-link-19','/nologo','/driver','/subsystem:native,3.10','/osversion:3.10','/entry:DriverEntry@8','/nodefaultlib','/safeseh:no','/machine:x86','/release','/base:0x10000','/out:'+str(out/'XMMGUARD.SYS'),out/'guard.obj',out/'guard_asm.obj',tools/'i686-w64-mingw32/lib/libntoskrnl.a'])
compile_c(project/'test/load_guard.c',out/'load_guard.obj')
run([tools/'bin/lld-link-19','/nologo','/entry:entry@0','/subsystem:windows,3.10','/osversion:3.10','/nodefaultlib','/safeseh:no','/machine:x86','/out:'+str(out/'LOADXMM.EXE'),out/'load_guard.obj',tools/'i686-w64-mingw32/lib/libkernel32.a',tools/'i686-w64-mingw32/lib/libadvapi32.a'])
sys.path.insert(0,str(project/'scripts'))
from pad_legacy_pe import pad_image
print(pad_image(out/'LOADXMM.EXE'))
