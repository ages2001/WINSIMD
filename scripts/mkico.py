import struct

W=H=32
# 4bpp XOR bitmap (bottom-up), 1bpp AND mask. Red X on transparent background.
# palette: 0=black(unused as visible), 1=red, 2=dark-red (edge/shadow)
pal=[(0,0,0),(255,0,0),(128,0,0)]+[(0,0,0)]*13  # (R,G,B)

def on_x(x,y):
    # thick diagonals, with a margin border so it reads as an X not a full square
    m=3
    if x<m or x>=W-m or y<m or y>=H-m: return 0
    d1=abs(x-y); d2=abs(x-(H-1-y))
    if d1<=2 or d2<=2: return 1          # core red
    if d1<=3 or d2<=3: return 2          # dark-red edge
    return 0

# build per-pixel index (top-down logical), and transparency
idx=[[0]*W for _ in range(H)]
trans=[[1]*W for _ in range(H)]   # 1 = transparent
for y in range(H):
    for x in range(W):
        v=on_x(x,y)
        if v:
            idx[y][x]=v
            trans[y][x]=0

# XOR 4bpp bottom-up
xor=bytearray()
for y in range(H-1,-1,-1):
    row=bytearray()
    for x in range(0,W,2):
        hi=idx[y][x]&0xF; lo=idx[y][x+1]&0xF
        row.append((hi<<4)|lo)
    xor+=row
# AND mask 1bpp bottom-up, rows padded to 4 bytes (4 bytes already for 32px)
andm=bytearray()
for y in range(H-1,-1,-1):
    bits=0; nb=0; row=bytearray()
    for x in range(W):
        bits=(bits<<1)|(trans[y][x]&1); nb+=1
        if nb==8: row.append(bits); bits=0; nb=0
    andm+=row

# BITMAPINFOHEADER: height = 2*H (XOR+AND)
bih=struct.pack('<IiiHHIIiiII',40,W,H*2,1,4,0,0,0,0,0,0)
palb=b''.join(struct.pack('<BBBB',b,g,r,0) for (r,g,b) in pal)  # RGBQUAD = B,G,R,0
dib=bih+palb+bytes(xor)+bytes(andm)

# ICONDIR + ICONDIRENTRY
icondir=struct.pack('<HHH',0,1,1)
offset=6+16
entry=struct.pack('<BBBBHHII',W,H,16,0,1,4,len(dib),offset)
ico=icondir+entry+dib
open('uninst.ico','wb').write(ico)
print("ico bytes:",len(ico))

# emit C array
with open('uninst_ico.h','w') as f:
    f.write("/* uninst_ico.h - generated 32x32 16-color icon: red X, transparent bg.\n")
    f.write("   Classic .ico (4bpp + 1bpp AND mask) for NT 3.x Program Manager items,\n")
    f.write("   which do not support 32bpp alpha icons. Regenerate: scripts/mkico.py */\n")
    f.write("static const unsigned char UNINST_ICO[] = {\n")
    for i in range(0,len(ico),12):
        f.write("  "+",".join("0x%02x"%b for b in ico[i:i+12])+",\n")
    f.write("};\n")
    f.write("#define UNINST_ICO_LEN %d\n"%len(ico))
print("wrote uninst_ico.h")
