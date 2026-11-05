"""Build the diagnostic XMM guard with its versioned readiness query."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import pefile

project=Path(__file__).resolve().parents[1]
root=project.parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel',type=Path,default=root/'fp32/reference/nt31kernel/NTOSKRNL.EXE',
                    help='Original NT31 kernel used to validate native import names; not bundled')
args=parser.parse_args()
if not args.kernel.is_file():
    parser.error('Supply --kernel with the original NT31 kernel path for import validation')
tools=Path(os.environ.get('WINSIMD_TOOLS',str(root/'tools/usr'))).resolve()
out=project/'out/query-candidate'
out.mkdir(parents=True,exist_ok=True)
def run(args):subprocess.run([str(x) for x in args],check=True)
for name in ['nt31_xmm_guard','nt31_guard_query']:
    run([tools/'bin/clang-19','--target=i686-w64-windows-gnu','-isystem',tools/'share/mingw-w64/include',
         '-isystem',tools/'share/mingw-w64/include/ddk',
         '-march=i386','-mno-sse','-mno-sse2','-mno-x87','-ffreestanding','-fno-stack-protector',
         '-fno-builtin','-Os','-DGUARD_QUERY','-c',project/'src'/(name+'.c'),'-o',out/(name+'.obj')])
run([tools/'bin/nasm','-f','win32',project/'src/nt31_xmm_guard.asm','-o',out/'guard_asm.obj'])
(out/'nt31-complete.def').write_text('LIBRARY ntoskrnl.exe\nEXPORTS\nNt31CompleteRequest@8=IoCompleteRequest\n')
run([tools/'bin/i686-w64-mingw32-dlltool','-m','i386','--kill-at','-d',out/'nt31-complete.def','-l',out/'nt31-complete.lib'])
target=out/'XMMGUARD.SYS'
run([tools/'bin/lld-link-19','/nologo','/driver','/subsystem:native,3.10','/osversion:3.10',
     '/entry:DriverEntry@8','/nodefaultlib','/safeseh:no','/machine:x86','/release','/base:0x10000',
     '/out:'+str(target),out/'nt31_xmm_guard.obj',out/'nt31_guard_query.obj',out/'guard_asm.obj',
     out/'nt31-complete.lib',tools/'i686-w64-mingw32/lib/libntoskrnl.a'])
p=pefile.PE(str(target))
# llvm-dlltool's alias is a link alias; use the verified native export on disk.
for lib in p.DIRECTORY_ENTRY_IMPORT:
    for entry in lib.imports:
        if entry.name==b'Nt31CompleteRequest':
            p.set_bytes_at_rva(entry.hint_name_table_rva+2,b'IoCompleteRequest\0')
p.OPTIONAL_HEADER.CheckSum=p.generate_checksum()
p.write(str(target))
p=pefile.PE(str(target))
kernel=pefile.PE(str(args.kernel))
exports={x.name for x in kernel.DIRECTORY_ENTRY_EXPORT.symbols}
imports=[x.name for lib in p.DIRECTORY_ENTRY_IMPORT for x in lib.imports]
assert all(x in exports for x in imports),imports
report={'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'native_imports':[x.decode() for x in imports],
        'native_validation':'pending','scope':'Versioned XMM/MXCSR context-preservation readiness query; no YMM/ZMM claim.'}
(out/'build.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
