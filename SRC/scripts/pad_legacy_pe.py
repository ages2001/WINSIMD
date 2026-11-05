"""Materialize zero-filled PE section tails for legacy NT image loaders.
Keeps RVAs and all existing section bytes. Rebuilds raw layout on page boundaries.
This invalidates any original signature; modified executables must not claim one.
"""
import pefile,math
from pathlib import Path
def pad_image(path):
 path=Path(path);p=pefile.PE(str(path));original=path.read_bytes();alignment=max(p.OPTIONAL_HEADER.SectionAlignment,p.OPTIONAL_HEADER.FileAlignment)
 align=lambda x:(x+alignment-1)//alignment*alignment
 offset=align(p.OPTIONAL_HEADER.SizeOfHeaders);chunks=[];ranges=[]
 for s in p.sections:
  old=s.PointerToRawData;oldsize=s.SizeOfRawData;size=align(max(oldsize,s.Misc_VirtualSize))
  content=original[old:old+oldsize] if oldsize else b''
  if len(content)!=oldsize:raise ValueError('Truncated section')
  chunks.append((offset,content+bytes(size-oldsize)));ranges.append((old,oldsize,offset))
  s.PointerToRawData=offset;s.SizeOfRawData=size;offset+=size
 for debug in getattr(p,'DIRECTORY_ENTRY_DEBUG',[]):
  ptr=debug.struct.PointerToRawData
  for old,size,new in ranges:
   if old<=ptr<old+size:debug.struct.PointerToRawData=new+ptr-old;break
 p.OPTIONAL_HEADER.DATA_DIRECTORY[4].VirtualAddress=0;p.OPTIONAL_HEADER.DATA_DIRECTORY[4].Size=0
 header=p.write()[:p.OPTIONAL_HEADER.SizeOfHeaders];result=bytearray(offset);result[:len(header)]=header
 # p.write includes updated debug directory structures; use its section contents.
 changed=p.write()
 for (old,oldsize,new),(off,chunk) in zip(ranges,chunks):
  if oldsize:chunk=changed[old:old+oldsize]+chunk[oldsize:]
  result[off:off+len(chunk)]=chunk
 q=pefile.PE(data=bytes(result));q.OPTIONAL_HEADER.CheckSum=q.generate_checksum();q.write(str(path))
 # Verify original bytes except deliberately changed debug directory metadata.
 for s,(old,oldsize,new) in zip(q.sections,ranges):
  assert s.PointerToRawData%alignment==0
  if s.SizeOfRawData>oldsize:
   assert set(q.get_data(s.VirtualAddress+oldsize,s.SizeOfRawData-oldsize))<={0}
 return {'path':str(path),'old_size':len(original),'new_size':path.stat().st_size}
if __name__=='__main__':
 import sys,json
 print(json.dumps([pad_image(p) for p in sys.argv[1:]],indent=2))
