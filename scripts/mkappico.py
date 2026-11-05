import struct
# 16-color palette (R,G,B). idx: 0 transparent-black,1 frame,2 bg,3 red,4 green,5 blue,6 yellow,7 white
pal=[(0,0,0),(40,70,120),(24,32,52),(230,70,60),(60,190,90),(70,130,230),(235,200,60),(235,235,245)]
pal+=[(0,0,0)]*(16-len(pal))

def design(S):
    idx=[[2]*S for _ in range(S)]     # bg
    trans=[[0]*S for _ in range(S)]
    # frame border
    for x in range(S):
        idx[0][x]=idx[S-1][x]=1; idx[x][0]=idx[x][S-1]=1
    # 4 SIMD lanes (colored bars of varying length)
    colors=[3,4,5,6]
    m=max(2,S//8)
    laneh=max(1,(S-2*m)//7)
    gap=laneh
    lengths=[0.85,0.55,0.95,0.70]
    y=m
    for i,c in enumerate(colors):
        x0=m
        x1=m+int((S-2*m)*lengths[i])
        for yy in range(y,min(y+laneh,S-1)):
            for xx in range(x0,min(x1,S-1)):
                idx[yy][xx]=c
        y+=laneh+gap
        if y>=S-1: break
    return idx,trans

def ico_entry(S):
    idx,trans=design(S)
    # XOR 4bpp bottom-up, rows padded to 4 bytes
    stride=((S*4+31)//32)*4
    xor=bytearray()
    for yy in range(S-1,-1,-1):
        row=bytearray(stride)
        for xx in range(S):
            v=idx[yy][xx]&0xF
            if xx&1: row[xx//2]=(row[xx//2]&0xF0)|v
            else:    row[xx//2]=(row[xx//2]&0x0F)|(v<<4)
        xor+=row
    # AND mask 1bpp (all opaque -> 0), rows padded to 4 bytes
    mstride=((S+31)//32)*4
    andm=bytearray(mstride*S)
    bih=struct.pack('<IiiHHIIiiII',40,S,S*2,1,4,0,0,0,0,0,0)
    palb=b''.join(struct.pack('<BBBB',b,g,r,0) for (r,g,b) in pal)
    return bih+palb+bytes(xor)+bytes(andm)

sizes=[32,16]
dibs=[ico_entry(s) for s in sizes]
n=len(sizes)
hdr=struct.pack('<HHH',0,1,n)
off=6+16*n
entries=b''
for s,d in zip(sizes,dibs):
    entries+=struct.pack('<BBBBHHII',s,s,16,0,1,4,len(d),off); off+=len(d)
ico=hdr+entries+b''.join(dibs)
open('winsimd.ico','wb').write(ico)
print("winsimd.ico",len(ico),"bytes,",n,"sizes")
