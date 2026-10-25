#!/usr/bin/env python3
# ctxcalls.py <pe> <lo> <hi> : vtable calls in [lo,hi) whose object was loaded from [reg+OFF] a few instructions before; prints OFF -> slots histogram (capstone: re-tools/angrenv has it)
import sys,struct,re,capstone,collections
path,lo,hi=sys.argv[1],int(sys.argv[2],16),int(sys.argv[3],16)
d=open(path,'rb').read(); pe=struct.unpack_from('<I',d,0x3c)[0]; ns=struct.unpack_from('<H',d,pe+6)[0]; so=struct.unpack_from('<H',d,pe+20)[0]
for i in range(ns):
    sh=pe+24+so+i*40; vs,va,rs,rp=struct.unpack_from('<IIII',d,sh+8)
    if va<=lo<va+vs: break
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.skipdata=True
ins=list(md.disasm(d[rp+lo-va:rp+hi-va],lo))
H=collections.defaultdict(collections.Counter); W=collections.defaultdict(list)
for k,x in enumerate(ins):
    if x.mnemonic!='call': continue
    m=re.match(r'qword ptr \[((?!rip)r\w+)(?: \+ (0x[0-9a-f]+|\d+))?\]$',x.op_str)
    if not m: continue
    slot=int(m.group(2) or '0',0)//8; vreg=m.group(1)
    # find "mov vreg, qword ptr [rcx]" then "mov rcx, qword ptr [R + OFF]"
    obj=None
    for j in range(k-1,max(0,k-12),-1):
        y=ins[j]
        if y.mnemonic=='mov' and y.op_str.startswith(vreg+', qword ptr [') :
            src=re.match(vreg+r', qword ptr \[(r\w+)\]$',y.op_str)
            if src:
                oreg=src.group(1)
                for jj in range(j-1,max(0,j-12),-1):
                    z=ins[jj]; mm=re.match(oreg+r', qword ptr \[r\w+ \+ (0x[0-9a-f]+)\]$',z.op_str)
                    if z.mnemonic=='mov' and mm: obj=mm.group(1); break
                    if z.op_str.startswith(oreg+','): break
            break
    H[obj][slot]+=1; W[(obj,slot)].append(x.address)
for o in sorted(H,key=lambda s:(s is None,s or '')):
    print(o, ' '.join(f'{s}:{c}' for s,c in sorted(H[o].items())))
