#!/usr/bin/env python3
# slotdis.py <pe> <rva_from> <rva_to> : disassembly with "slot N" on every call [reg+off]
import sys,struct,re,capstone
path,a,b=sys.argv[1],int(sys.argv[2],16),int(sys.argv[3],16)
d=open(path,'rb').read(); pe=struct.unpack_from('<I',d,0x3c)[0]; ns=struct.unpack_from('<H',d,pe+6)[0]; so=struct.unpack_from('<H',d,pe+20)[0]
for i in range(ns):
    sh=pe+24+so+i*40; vs,va,rs,rp=struct.unpack_from('<IIII',d,sh+8)
    if va<=a<va+vs:
        md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
        for ins in md.disasm(d[rp+a-va:rp+b-va],a):
            s=f'{ins.address:8x}  {ins.mnemonic:6s} {ins.op_str}'
            m=re.match(r'qword ptr \[(?!rip)r\w+(?: \+ (0x[0-9a-f]+|\d+))?\]$',ins.op_str)
            if ins.mnemonic=='call' and m: s+=f'   ; slot {int(m.group(1) or "0",0)//8}'
            if 'rip +' in ins.op_str:
                mm=re.search(r'rip \+ (0x[0-9a-f]+)',ins.op_str); t=ins.address+ins.size+int(mm.group(1),16); s+=f'   ; ->{t:x}'
            print(s)
