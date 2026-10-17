#!/usr/bin/env python3
# pe2le.py - convert a 32-bit PE image (lld-link /dll output, all sections
# merged into one) into a Windows 9x LE VxD that Win95's VMM loader accepts.
#   pe2le.py <in.dll> <out.vxd> [--flags=0x2047]
#
# - The PE's sections are laid out flat by RVA into a single LE object
#   (class LCOD, preload, 32-bit; readable/writable/executable by default).
# - Every IMAGE_REL_BASED_HIGHLOW base relocation becomes an LE internal
#   32-bit-offset fixup into that object (page-crossing ones get the extra
#   negative-offset record LE requires).
# - The DDB is found by its name 'WINSIMD ' + SDK 4.0 tail and exported as
#   ordinal 1 (that's how VMM finds a VxD's DDB).
# Loader rules learnt on Win95 RTM (BOOTLOG says only "LoadFailed"):
#   1. the DDB must carry the SDK 4.0 tail ('Prev', 50h, 'Rsv1'..'Rsv3');
#   2. only one preloaded object (stock VxDs preload just their LCOD object).
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lewrite

PAGESZ = 0x1000
src, out = sys.argv[1], sys.argv[2]
flags = 0x2047
for a in sys.argv[3:]:
    if a.startswith('--flags='): flags = int(a.split('=')[1], 0)

pe = open(src, 'rb').read()
lfa = struct.unpack_from('<I', pe, 0x3c)[0]
assert pe[lfa:lfa+4] == b'PE\0\0'
machine, nsec, _, _, _, optsz, _ = struct.unpack_from('<HHIIIHH', pe, lfa + 4)
assert machine == 0x14c, 'need an i386 PE'
opt = lfa + 24
assert struct.unpack_from('<H', pe, opt)[0] == 0x10b, 'need PE32'
imagebase = struct.unpack_from('<I', pe, opt + 28)[0]
reloc_rva, reloc_sz = struct.unpack_from('<II', pe, opt + 96 + 5 * 8)

secs = []
for i in range(nsec):
    o = opt + optsz + i * 40
    name = pe[o:o+8].rstrip(b'\0').decode()
    vsz, va, rsz, rptr = struct.unpack_from('<IIII', pe, o + 8)
    secs.append((name, va, vsz, rsz, rptr))
body = [s for s in secs if s[0] != '.reloc']
start = min(s[1] for s in body)
end = max(s[1] + max(s[2], s[3]) for s in body)
img = bytearray(end - start)
for name, va, vsz, rsz, rptr in body:
    n = min(rsz, vsz) if vsz else rsz
    img[va - start: va - start + n] = pe[rptr: rptr + n]

def rva2off(rva):
    for name, va, vsz, rsz, rptr in secs:
        if va <= rva < va + max(vsz, rsz): return rptr + rva - va
    raise ValueError(hex(rva))

# base relocations -> list of flat offsets holding absolute addresses
rel = []
if reloc_sz:
    p = rva2off(reloc_rva); e = p + reloc_sz
    while p < e:
        page, bsz = struct.unpack_from('<II', pe, p)
        if bsz == 0: break
        for k in range((bsz - 8) // 2):
            ent = struct.unpack_from('<H', pe, p + 8 + 2 * k)[0]
            typ, off = ent >> 12, ent & 0xFFF
            if typ == 0: continue
            assert typ == 3, f'unsupported reloc type {typ}'
            rel.append(page + off - start)
        p += bsz

npages = (len(img) + PAGESZ - 1) // PAGESZ
img = img.ljust(npages * PAGESZ, b'\0')
recs = [b''] * npages

def fixrec(srcoff, target):
    if target < 0x10000:
        return bytes([0x07, 0x00]) + struct.pack('<h', srcoff) + bytes([1]) + struct.pack('<H', target)
    return bytes([0x07, 0x10]) + struct.pack('<h', srcoff) + bytes([1]) + struct.pack('<I', target)

for f in sorted(rel):
    v = struct.unpack_from('<I', img, f)[0]
    t = v - imagebase - start
    assert 0 <= t < len(img), f'reloc at {f:#x} points outside image ({v:#x})'
    struct.pack_into('<I', img, f, t)          # cosmetic: loader overwrites it
    pg, po = divmod(f, PAGESZ)
    recs[pg] += fixrec(po, t)
    if po > PAGESZ - 4:                       # dword straddles into next page
        recs[pg + 1] += fixrec(po - PAGESZ, t)

# DDB
i = 0
while True:
    i = img.index(b'WINSIMD ', i)
    ddb = i - 0x0c
    if ddb >= 0 and img[ddb+0x3c:ddb+0x40] == b'verP' and img[ddb+0x40] == 0x50: break
    i += 1
assert ddb % 4 == 0

# DOS stub: prints a message and exits; e_lfanew -> 0x80
msg = b'Windows 9x VxD.\r\n$'
code = bytes([0x0E, 0x1F, 0xBA, 0x0E, 0x00, 0xB4, 0x09, 0xCD, 0x21, 0xB8, 0x01, 0x4C, 0xCD, 0x21]) + msg
mz = bytearray(0x40)
struct.pack_into('<2sHHHHHHHHHHHHH', mz, 0, b'MZ', 0x80, 1, 0, 4, 0x10, 0xFFFF, 0, 0xB8, 0, 0, 0, 0x40, 0)
struct.pack_into('<I', mz, 0x3c, 0x80)
dos = (bytes(mz) + code).ljust(0x80, b'\0')

resn   = bytes([7]) + b'WINSIMD' + struct.pack('<H', 0) + b'\0'
ent    = bytes([1, 3]) + struct.pack('<H', 1) + bytes([3]) + struct.pack('<I', ddb) + b'\0'
nonres = bytes([0x17]) + b' WINSIMD Virtual Device' + struct.pack('<H', 0) + b'\0'

le = lewrite.write_le(dos, [(bytes(img), flags, b'LCOD')], resn, ent, recs, nonres, b'',
                      devid=0, ddkver=0x400)
open(out, 'wb').write(le)
print(f'{out}: {len(le)} bytes, image {end-start:#x} ({npages} page(s)), '
      f'{len(rel)} fixups, DDB @{ddb:#x}, ctrl @{struct.unpack_from("<I", img, ddb+0x18)[0]:#x}, flags {flags:#x}')
