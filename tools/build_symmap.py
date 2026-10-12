#!/usr/bin/env python3
# Build a function -> Rust source-file map for a PE by mining embedded panic
# Location structs ({file_ptr,file_len,line,col}) referenced by each .pdata
# function. No PDB needed. Usage: build_symmap.py <pe> <imgbase_hex> > map.tsv
import struct,sys
from collections import Counter
try:
    from capstone import Cs,CS_ARCH_X86,CS_MODE_64
except ImportError:
    sys.exit("pip3 install capstone")
path=sys.argv[1]; IMG=int(sys.argv[2],16)
f=open(path,'rb').read()
pe=struct.unpack('<I',f[0x3c:0x40])[0]; nsec=struct.unpack('<H',f[pe+6:pe+8])[0]; optsz=struct.unpack('<H',f[pe+20:pe+22])[0]
sectab=pe+24+optsz; secs=[]
for i in range(nsec):
    o=sectab+i*40; va,=struct.unpack('<I',f[o+12:o+16]); vsz,=struct.unpack('<I',f[o+8:o+12]); ptr,=struct.unpack('<I',f[o+20:o+24]); rsz,=struct.unpack('<I',f[o+16:o+20])
    secs.append((va,vsz,ptr,rsz))
def off(r):
    for va,vsz,ptr,rsz in secs:
        if va<=r<va+max(vsz,rsz): return ptr+(r-va)
    return None
def try_loc(rva):
    o=off(rva)
    if o is None or o+24>len(f): return None
    fp,fl,ln,col=struct.unpack('<QQII',f[o:o+24])
    if fl<4 or fl>200 or ln==0 or ln>400000 or col>8000 or fp<IMG or fp>IMG+0x2000000: return None
    fo=off(fp-IMG)
    if fo is None: return None
    s=f[fo:fo+fl]
    if not s.endswith(b'.rs'): return None
    try:
        t=s.decode('latin1')
        # shorten rustc std/core paths
        if 'library\\' in t or 'library/' in t: t='std:'+t.replace('\\','/').split('library/')[-1]
        elif 'index.crates.io' in t:
            t=t.replace('\\','/'); t='crate:'+t.split('index.crates.io-')[-1].split('/',1)[-1]
        else: t=t.replace('\\','/')
        return t,ln
    except: return None
opt=pe+24; dd=opt+112; pr,ps=struct.unpack('<II',f[dd+3*8:dd+3*8+8]); po=off(pr)
md=Cs(CS_ARCH_X86,CS_MODE_64)
rows=[]
nf=ps//12
for i in range(nf):
    b,e,u=struct.unpack('<III',f[po+i*12:po+i*12+12])
    o=off(b)
    if o is None: continue
    code=f[o:o+(e-b)]; c=Counter()
    for ins in md.disasm(code,IMG+b):
        if ins.mnemonic=='lea' and 'rip' in ins.op_str:
            s=ins.op_str
            try:
                disp=int(s.split('rip + ')[1].split(']')[0],16) if 'rip + ' in s else -int(s.split('rip - ')[1].split(']')[0],16)
            except: continue
            loc=try_loc(ins.address+ins.size-IMG+disp)
            if loc: c[loc[0]]+=1
    # rank: prefer project/crate files over std
    def score(kv):
        name,cnt=kv; pri = 0 if name.startswith('std:') else (1 if name.startswith('crate:') else 2)
        return (pri,cnt)
    top=sorted(c.items(),key=score,reverse=True)[:3]
    files=";".join("%s(%d)"%(n,cnt) for n,cnt in top) if top else "-"
    rows.append((b,e,files))
print("# RVA_start\tRVA_end\tsize\tsource_files(ref_count)   [PE=%s IMG=0x%x funcs=%d]"%(path.split('/')[-1],IMG,len(rows)))
for b,e,files in rows:
    print("%x\t%x\t%d\t%s"%(b,e,e-b,files))
