#!/usr/bin/env python3
# memwrites.py <pe> <hexoffset>... : instructions in .text that touch [reg + off] (writes and reads)
import sys,struct,capstone,re
p=sys.argv[1]; offs=[int(x,16) for x in sys.argv[2:]]
d=open(p,'rb').read(); pe=struct.unpack_from('<I',d,0x3c)[0]; ns=struct.unpack_from('<H',d,pe+6)[0]; so=struct.unpack_from('<H',d,pe+20)[0]
secs=[]
for i in range(ns):
    sh=pe+24+so+i*40; nm=d[sh:sh+8].rstrip(b'\0'); vs,va,rs,rp=struct.unpack_from('<IIII',d,sh+8); secs.append((nm,va,vs,rp,rs))
nm,va,vs,rp,rs=[x for x in secs if x[0]==b'.text'][0]
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.skipdata=True
for ins in md.disasm(d[rp:rp+vs],va):
    for o in offs:
        if re.search(r'\[r\w+ \+ %s\]'%hex(o),ins.op_str): print(f'{ins.address:x} {ins.mnemonic} {ins.op_str}')
