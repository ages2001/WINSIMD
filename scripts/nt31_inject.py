#!/usr/bin/env python3
# nt31_inject.py - add a kernel-driver service key to a Windows NT 3.1 SYSTEM
# hive (regf v1.1). v1.1 cell layout: [size:i32][parent_cell_off:u32][node@+8];
# nk/vk names are UTF-16LE; subkey lists here are 'li' (plain, sorted, no hash).
#
#   nt31_inject.py <SYSTEM-hive> <out-hive> <ServiceName> <ImagePath|-> \
#                  [Start] [Type] [ErrorControl] [Group|-] [Tag]
# defaults: Start=1 (system), Type=1 (kernel), ErrorControl=1, Group=Base, Tag=0
# ImagePath '-' omits it (NT derives \SystemRoot\System32\drivers\<svc>.sys).
#
# STATUS: produces a VALID, BOOTABLE hive - NT 3.1 boots to logon and the new
# key reads back correctly (all values, sk, hints match a stock service). It
# reproduces every documented nk/vk field of a working service (Beep), reuses a
# sibling service's security descriptor, sets the max-value hints, and appends a
# well-formed hbin. HOWEVER, in testing NT 3.1's boot-time driver auto-start did
# NOT pick up the injected key, tracked to an undocumented v1.1 cell field (the
# 4 bytes between a cell's size and its node; stock cells carry a position-
# dependent value we could not reverse out). The driver ITSELF is proven to run
# on NT 3.1 by overwriting a stock system-start driver file (see test/results/
# nt31_driver_enabled_e9.log). On real NT 3.1, install via ilfinst.exe, which
# creates the service through NT's own config manager and sidesteps all of this.
# This script remains a working v1.1 hive reader/parser and near-complete writer.
import sys, struct

BASE = 0x1000
def u16(d,o): return struct.unpack('<H',d[o:o+4][:2])[0]
def u32(d,o): return struct.unpack('<I',d[o:o+4])[0]
def i32(d,o): return struct.unpack('<i',d[o:o+4])[0]
def node(off): return BASE+off+8          # cell offset (hbin-rel) -> node file off
def rel(fileoff): return fileoff-BASE     # file off of cell -> stored offset

def wname(d,n):
    return d[n+0x4c:n+0x4c+u16(d,n+0x48)].decode('utf-16-le','replace')

def sub_offsets(d,n):
    sl=u32(d,n+0x1c)
    if sl in (0,0xffffffff): return []
    ln=node(sl); sig=d[ln:ln+2]; cnt=u16(d,ln+2)
    if sig==b'li':  return [u32(d,ln+4+i*4) for i in range(cnt)]
    if sig in (b'lf',b'lh'): return [u32(d,ln+4+i*8) for i in range(cnt)]
    if sig==b'ri':
        out=[]
        for i in range(cnt):
            sn=node(d and u32(d,ln+4+i*4));
            for j in range(u16(d,sn+2)): out.append(u32(d,sn+4+j*8))
        return out
    return []

def find_sub(d,n,name):
    for o in sub_offsets(d,n):
        if d[node(o):node(o)+2]==b'nk' and wname(d,node(o)).lower()==name.lower():
            return o
    return None

def main():
    hive,out,svc,img = sys.argv[1],sys.argv[2],sys.argv[3],sys.argv[4]
    start = int(sys.argv[5]) if len(sys.argv)>5 else 1
    styp  = int(sys.argv[6]) if len(sys.argv)>6 else 1
    errc  = int(sys.argv[7]) if len(sys.argv)>7 else 1
    group = sys.argv[8] if len(sys.argv)>8 else 'Base'   # '' to omit
    if group=='-': group=''
    tag   = int(sys.argv[9]) if len(sys.argv)>9 else 0   # 0 => omit Tag

    d=bytearray(open(hive,'rb').read())
    assert d[0:4]==b'regf', "not a regf hive"

    root=node(u32(d,0x24))
    cs=find_sub(d,root,'ControlSet001'); assert cs is not None
    svc_off=find_sub(d,node(cs),'Services'); assert svc_off is not None
    svcn=node(svc_off)
    # sanity: layout offsets correct? a known child's parent must equal svc cell off
    kids=sub_offsets(d,svcn)
    beep=find_sub(d,svcn,'Beep')
    assert beep is not None and u32(d,node(beep)+0x10)==svc_off, "nk layout/parent mismatch"
    sk = u32(d,node(beep)+0x2c)           # reuse a SIBLING SERVICE's security
                                          # descriptor (all services share one;
                                          # it grants the load-time access the
                                          # Services container's sk does not)

    sl=u32(d,svcn+0x1c); licell=BASE+sl; lisize=abs(i32(d,licell))
    linode=node(sl)                        # 'li' node is at cell+8
    assert d[linode:linode+2]==b'li', "expected 'li' subkey list"
    licount=u16(d,linode+2)                # count at node+2
    li_used = 8 + 4 + licount*4            # cellhdr + (sig+count) + entries
    need = li_used + 4
    assert need <= lisize, f"li cell full ({li_used}/{lisize}); realloc not implemented"

    # ---- build a new hbin appended at end of file ----
    end_rel = u32(d,0x28)                  # end-of-hbins (rel); == filelen-BASE
    hbin_rel = end_rel
    hbin_fileoff = BASE+hbin_rel
    assert hbin_fileoff==len(d), "hive not contiguous"
    HBIN=0x1000
    blob=bytearray(HBIN)
    blob[0:4]=b'hbin'
    struct.pack_into('<I',blob,0x04,hbin_rel)
    struct.pack_into('<I',blob,0x08,HBIN)
    struct.pack_into('<I',blob,0x1c,HBIN)      # spare field: match regular hbins
    # cursor within blob, cells start after 0x20 header
    cur=0x20
    cells={}   # name -> (rel_off, node_fileoff_in_final)
    def alloc(size, parent_rel):
        nonlocal cur
        size=(size+7)&~7
        off=cur
        struct.pack_into('<i',blob,off,-size)                 # negative = allocated
        struct.pack_into('<I',blob,off+4,parent_rel & 0xffffffff)
        cur+=size
        return off                                            # offset within blob
    def blob_rel(bloboff): return hbin_rel + bloboff          # -> stored offset
    def blob_node(bloboff): return bloboff+8                  # node within blob

    namew = svc.encode('utf-16-le')
    want_imagepath = img not in ('', '-')     # '-' or '' => omit (NT derives
                                              #   \SystemRoot\System32\drivers\<svc>.sys)
    imgw  = (img+'\x00').encode('utf-16-le') if want_imagepath else b''
    groupw = (group+'\x00').encode('utf-16-le') if group else b''

    # 1) nk cell (fill parent later-known = svc_off)
    nk_sz = 0x4c + len(namew)
    nk = alloc(nk_sz+8, svc_off)                              # +8 for cell header
    # 2) data cells for string values (only those used)
    if want_imagepath:
        dc = alloc(8+len(imgw), 0)   # parent set to imagepath vk later
        struct.pack_into(f'<{len(imgw)}s',blob,blob_node(dc),imgw)
    if group:
        gdc = alloc(8+len(groupw), 0)
        struct.pack_into(f'<{len(groupw)}s',blob,blob_node(gdc),groupw)
    # 3) vk cells
    maxnm=[0]; maxdl=[0]
    def mk_vk(name, dtype, inline_dword=None, data_rel=None, data_len=None, parent_rel=0):
        nw=name.encode('utf-16-le')
        if len(nw)>maxnm[0]: maxnm[0]=len(nw)
        dl_here = 4 if inline_dword is not None else data_len
        if dl_here>maxdl[0]: maxdl[0]=dl_here
        vk=alloc(8+0x14+len(nw), parent_rel)      # 8 cell hdr + 0x14 vk + name
        n=blob_node(vk)
        blob[n:n+2]=b'vk'
        struct.pack_into('<H',blob,n+2,len(nw))
        if inline_dword is not None:
            struct.pack_into('<I',blob,n+4,0x80000004)        # inline, 4 bytes
            struct.pack_into('<I',blob,n+8,inline_dword & 0xffffffff)
            struct.pack_into('<I',blob,n+0x0c,4)              # REG_DWORD
        else:
            struct.pack_into('<I',blob,n+4,data_len)
            struct.pack_into('<I',blob,n+8,data_rel)
            struct.pack_into('<I',blob,n+0x0c,1)              # REG_SZ
        struct.pack_into('<H',blob,n+0x10,0)                  # flags (wide name)
        blob[n+0x14:n+0x14+len(nw)]=nw
        return vk
    nk_rel = blob_rel(nk)
    vk_type = mk_vk('Type',4, inline_dword=styp, parent_rel=nk_rel)
    vk_start= mk_vk('Start',4, inline_dword=start, parent_rel=nk_rel)
    vk_err  = mk_vk('ErrorControl',4, inline_dword=errc, parent_rel=nk_rel)
    vklist=[vk_type,vk_start,vk_err]
    if tag:
        vklist.append(mk_vk('Tag',4, inline_dword=tag, parent_rel=nk_rel))
    if group:
        vk_grp = mk_vk('Group',1, data_rel=blob_rel(gdc), data_len=len(groupw), parent_rel=nk_rel)
        struct.pack_into('<I',blob,gdc+4,blob_rel(vk_grp))     # data cell parent -> vk
        vklist.append(vk_grp)
    if want_imagepath:
        vk_img  = mk_vk('ImagePath',1, data_rel=blob_rel(dc), data_len=len(imgw), parent_rel=nk_rel)
        struct.pack_into('<I',blob,dc+4,blob_rel(vk_img))   # data cell parent -> vk
        vklist.append(vk_img)
    # 4) value-list cell
    vlist = alloc(8+len(vklist)*4, nk_rel)
    vn=blob_node(vlist)
    for i,vk in enumerate(vklist):
        struct.pack_into('<I',blob,vn+i*4,blob_rel(vk))
    # 5) fill the nk node
    n=blob_node(nk)
    blob[n:n+2]=b'nk'
    struct.pack_into('<H',blob,n+2,0x0000)      # flags: normal key, wide name
    struct.pack_into('<q',blob,n+4,0)           # timestamp
    struct.pack_into('<I',blob,n+0x10,svc_off)  # parent = Services
    struct.pack_into('<I',blob,n+0x14,0)        # nr_subkeys
    struct.pack_into('<I',blob,n+0x18,0)
    struct.pack_into('<I',blob,n+0x1c,0xffffffff)  # no subkey list
    struct.pack_into('<I',blob,n+0x20,0xffffffff)
    struct.pack_into('<I',blob,n+0x24,len(vklist))  # nr_values
    struct.pack_into('<I',blob,n+0x28,blob_rel(vlist))
    struct.pack_into('<I',blob,n+0x2c,sk)       # reuse Services' sk
    struct.pack_into('<I',blob,n+0x30,0xffffffff)  # no classname
    struct.pack_into('<I',blob,n+0x34,0)        # max_subkey_name_len
    struct.pack_into('<I',blob,n+0x38,0)        # max_subkey_class_len
    struct.pack_into('<I',blob,n+0x3c,maxnm[0]) # max_value_name_len  (bytes)
    struct.pack_into('<I',blob,n+0x40,maxdl[0]) # max_value_data_len  (bytes)
    struct.pack_into('<I',blob,n+0x44,0)
    struct.pack_into('<H',blob,n+0x48,len(namew))
    struct.pack_into('<H',blob,n+0x4a,0)
    blob[n+0x4c:n+0x4c+len(namew)]=namew

    # 6) remaining space in hbin -> one free cell (positive size)
    if cur < HBIN:
        struct.pack_into('<i',blob,cur,HBIN-cur)   # positive = free

    # ---- append hbin, update header & Services ----
    d += blob

    # insert nk into Services 'li' list, sorted (case-insensitive by name)
    names=[wname(d,node(o)).lower() for o in kids]
    ins=0
    while ins<len(kids) and names[ins] < svc.lower(): ins+=1
    # shift entries [ins:] right by 4 within the li cell, write new offset
    base=linode+4                                       # entries at node+4
    old=bytes(d[base:base+licount*4])
    newlist=old[:ins*4]+struct.pack('<I',nk_rel)+old[ins*4:]
    d[base:base+len(newlist)]=newlist
    struct.pack_into('<H',d,linode+2,licount+1)         # li count++ (at node+2)
    struct.pack_into('<I',d,svcn+0x14,u32(d,svcn+0x14)+1)  # Services nr_subkeys++

    # end-of-hbins += HBIN
    struct.pack_into('<I',d,0x28,end_rel+HBIN)
    # bump sequence numbers (keep them equal = clean)
    seq=u32(d,4)+1
    struct.pack_into('<I',d,4,seq); struct.pack_into('<I',d,8,seq)
    # recompute header checksum (xor of dwords [0,0x1fc))
    x=0
    for o in range(0,0x1fc,4): x^=u32(d,o)
    if x==0: x=1
    if x==0xffffffff: x=0xfffffffe
    struct.pack_into('<I',d,0x1fc,x)

    open(out,'wb').write(d)
    print(f"injected '{svc}' -> {img}  (nk_rel=0x{nk_rel:x}, li now {licount+1}, +1 hbin)")
    print(f"out={out} size={len(d)}")

main()
