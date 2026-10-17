#!/usr/bin/env python3
# ledump.py - dump a Windows 9x LE (VxD) file: header, objects, page map,
# entry table, fixups, DDB.   ledump.py file.vxd [-v]
import struct, sys

def u8(b, o): return b[o]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]

HDR = [(0x08,'cpu',2),(0x0a,'os',2),(0x0c,'modver',4),(0x10,'mflags',4),(0x14,'npages',4),
       (0x18,'eipobj',4),(0x1c,'eip',4),(0x20,'espobj',4),(0x24,'esp',4),(0x28,'pagesz',4),
       (0x2c,'lastpg',4),(0x30,'fixsz',4),(0x34,'fixcs',4),(0x38,'ldrsz',4),(0x3c,'ldrcs',4),
       (0x40,'objtab',4),(0x44,'nobj',4),(0x48,'pagemap',4),(0x4c,'iter',4),(0x50,'rsrctab',4),
       (0x54,'nrsrc',4),(0x58,'resnam',4),(0x5c,'entry',4),(0x60,'moddir',4),(0x64,'nmoddir',4),
       (0x68,'fpt',4),(0x6c,'frt',4),(0x70,'impmod',4),(0x74,'nimpmod',4),(0x78,'impproc',4),
       (0x7c,'pgcs',4),(0x80,'datapg',4),(0x84,'npreload',4),(0x88,'nonres',4),(0x8c,'nonreslen',4),
       (0x90,'nonrescs',4),(0x94,'autods',4),(0x98,'dbgoff',4),(0x9c,'dbglen',4),(0xa0,'ipreload',4),
       (0xa4,'idemand',4),(0xa8,'heap',4),(0xac,'r_ac',4),(0xb0,'r_b0',4),(0xb4,'r_b4',4),
       (0xb8,'winres',4),(0xbc,'winreslen',4),(0xc0,'devid',2),(0xc2,'ddkver',2)]

def parse(path):
    b = open(path, 'rb').read()
    le = u32(b, 0x3c)
    assert b[le:le+2] == b'LE', 'not LE'
    h = {name: (u16(b, le+o) if s == 2 else u32(b, le+o)) for o, name, s in HDR}
    h['_le'] = le; h['_b'] = b
    objs = []
    for i in range(h['nobj']):
        o = le + h['objtab'] + i*24
        vs, rb, fl, pmi, pmn, res = struct.unpack_from('<IIIII4s', b, o)
        objs.append(dict(vsize=vs, reloc=rb, flags=fl, pmidx=pmi, pmcnt=pmn, cls=res))
    h['_objs'] = objs
    pm = []
    for i in range(h['npages']):
        o = le + h['pagemap'] + i*4
        pm.append(b[o:o+4])
    h['_pm'] = pm
    return h

def page_file_off(h, page1):   # 1-based logical page
    return h['datapg'] + (page1-1)*h['pagesz']

def fixups(h):
    b, le = h['_b'], h['_le']
    out = []
    for p in range(h['npages']):
        s = u32(b, le+h['fpt']+p*4); e = u32(b, le+h['fpt']+(p+1)*4)
        o = le + h['frt'] + s; end = le + h['frt'] + e
        while o < end:
            src = b[o]; tf = b[o+1]; o += 2
            if src & 0x20:   # source list
                cnt = b[o]; o += 1; soffs = None
            else:
                soff = struct.unpack_from('<h', b, o)[0]; o += 2
            tt = tf & 3
            if tt == 0:
                if tf & 0x40: obj = u16(b, o); o += 2
                else: obj = b[o]; o += 1
                if (src & 0xf) == 2: toff = None
                elif tf & 0x10: toff = u32(b, o); o += 4
                else: toff = u16(b, o); o += 2
                tgt = ('int', obj, toff)
            else:
                tgt = ('ext', tt, b[o:o+6].hex()); o = end   # not expected in VxDs
            if src & 0x20:
                lst = [struct.unpack_from('<h', b, o+2*k)[0] for k in range(cnt)]; o += 2*cnt
                for so in lst: out.append((p+1, src, tf, so, tgt))
            else:
                out.append((p+1, src, tf, soff, tgt))
    return out

def entries(h):
    b, le = h['_b'], h['_le']
    o = le + h['entry']; ordn = 1; out = []
    while True:
        cnt = b[o]
        if cnt == 0: break
        typ = b[o+1]; o += 2
        if typ == 0: ordn += cnt; continue
        obj = u16(b, o); o += 2
        for k in range(cnt):
            if typ == 3:
                fl = b[o]; off = u32(b, o+1); o += 5
                out.append((ordn, typ, obj, fl, off))
            elif typ == 1:
                fl = b[o]; off = u16(b, o+1); o += 3
                out.append((ordn, typ, obj, fl, off))
            else:
                raise Exception('entry type %d' % typ)
            ordn += 1
    return out

def resnames(h):
    b, le = h['_b'], h['_le']
    o = le + h['resnam']; out = []
    while b[o]:
        n = b[o]; out.append((b[o+1:o+1+n].decode('latin1'), u16(b, o+1+n))); o += 3+n
    return out

def obj_image(h, i):          # 0-based object -> bytes of its pages (concatenated)
    b = h['_b']; ob = h['_objs'][i]; data = b''
    for k in range(ob['pmcnt']):
        lp = ob['pmidx'] + k
        e = h['_pm'][lp-1]
        pnum = (e[0] << 16) | (e[1] << 8) | e[2]    # LE: 3-byte big-endian page #
        off = page_file_off(h, pnum)
        sz = h['lastpg'] if pnum == h['npages'] else h['pagesz']
        data += b[off:off+sz].ljust(h['pagesz'], b'\0')
    return data[:max(ob['vsize'], 0)] if ob['vsize'] else data

def dump(path, verbose=False):
    h = parse(path); b = h['_b']
    print(f"== {path}  size={len(b)}  LE@0x{h['_le']:x}")
    print('  ' + ' '.join(f"{n}={h[n]:#x}" for _, n, _ in HDR))
    for i, ob in enumerate(h['_objs']):
        print(f"  obj{i+1}: vsize={ob['vsize']:#x} reloc={ob['reloc']:#x} flags={ob['flags']:#06x} "
              f"pm={ob['pmidx']}+{ob['pmcnt']} cls={ob['cls']}")
    print('  pagemap:', ' '.join(e.hex() for e in h['_pm']))
    print('  resnames:', resnames(h))
    ents = entries(h); print('  entries:', ents)
    fx = fixups(h)
    print(f"  fixups: {len(fx)}  (src types: {sorted(set(f[1] for f in fx))}, tflags: {sorted(set(f[2] for f in fx))})")
    if verbose:
        for f in fx[:40]: print('    ', f)
    if ents:
        ordn, typ, obj, fl, off = ents[0]
        img = obj_image(h, obj-1)
        d = img[off:off+0x50]
        print(f"  DDB @obj{obj}+{off:#x}: next={u32(d,0):#x} sdk={u16(d,4):#x} reqdev={u16(d,6):#x} "
              f"ver={d[8]}.{d[9]} flags={u16(d,10):#x} name={d[12:20]} init={u32(d,20):#x} "
              f"ctrl={u32(d,24):#x} v86={u32(d,28):#x} pm={u32(d,32):#x} svctab={u32(d,44):#x} svcsz={u32(d,48):#x}")
        ddbfx = [f for f in fx if f[3] is not None]
        # fixups landing inside the DDB
        pg0 = h['_objs'][obj-1]['pmidx']
        inddb = [f for f in fx if f[0] == pg0 + off//h['pagesz'] and off%h['pagesz'] <= f[3] < off%h['pagesz']+0x50]
        print('  DDB fixups:', [(hex(f[3]-off%h['pagesz']), f[4]) for f in inddb])
    return h

if __name__ == '__main__':
    for p in sys.argv[1:]:
        if p.startswith('-'): continue
        dump(p, '-v' in sys.argv)
