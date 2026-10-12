#!/usr/bin/env python3
"""Reusable PE RE helpers for Reclaimer debugging.
   from pe import PE ; p=PE('exe'|'halo3') ; p.dis(rva,len) ; p.sym(rva) ; p.loc(rva)
"""
import struct,bisect,os
from capstone import Cs,CS_ARCH_X86,CS_MODE_64
H3=os.path.expanduser("~/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection/halo3/halo3.dll")
EXE=os.path.join(os.path.dirname(__file__),"..","analysis","project-reclaimer-v0.9.13.exe")
SYMMAP=os.path.join(os.path.dirname(__file__),"..","analysis","reclaimer-exe-symmap.tsv")
class PE:
    def __init__(self,which):
        self.which=which
        path = EXE if which=='exe' else H3
        self.f=open(path,'rb').read()
        pe=struct.unpack('<I',self.f[0x3c:0x40])[0]
        nsec=struct.unpack('<H',self.f[pe+6:pe+8])[0]; optsz=struct.unpack('<H',self.f[pe+20:pe+22])[0]
        self.img=struct.unpack('<Q',self.f[pe+24+24:pe+24+32])[0]
        st=pe+24+optsz; self.secs=[]
        for i in range(nsec):
            o=st+i*40; va,=struct.unpack('<I',self.f[o+12:o+16]); vsz,=struct.unpack('<I',self.f[o+8:o+12]); ptr,=struct.unpack('<I',self.f[o+20:o+24]); rsz,=struct.unpack('<I',self.f[o+16:o+20])
            self.secs.append((va,vsz,ptr,rsz))
        dd=pe+24+112
        self.imp=struct.unpack('<II',self.f[dd+8:dd+16])
        self.md=Cs(CS_ARCH_X86,CS_MODE_64)
        self.base = 0x140000000 if which=='exe' else 0x180000000
        self._symrows=None
    def off(self,rva):
        for va,vsz,ptr,rsz in self.secs:
            if va<=rva<va+max(vsz,rsz): return ptr+(rva-va)
        return None
    def rd(self,rva,n): o=self.off(rva); return self.f[o:o+n] if o is not None else b''
    def u32(self,rva): return struct.unpack('<I',self.rd(rva,4))[0]
    def u64(self,rva): return struct.unpack('<Q',self.rd(rva,8))[0]
    def _loadsym(self):
        if self._symrows is not None: return
        self._symrows=[]
        if self.which=='exe' and os.path.exists(SYMMAP):
            for line in open(SYMMAP):
                if line.startswith('#'): continue
                p=line.rstrip('\n').split('\t')
                if len(p)>=4: self._symrows.append((int(p[0],16),int(p[1],16),p[3]))
            self._symrows.sort()
        self._starts=[r[0] for r in self._symrows]
    def sym(self,rva):
        self._loadsym()
        if not self._symrows: return "%s+0x%x"%(self.which,rva)
        i=bisect.bisect_right(self._starts,rva)-1
        if i>=0 and self._symrows[i][0]<=rva<self._symrows[i][1]:
            return "%s+0x%x[fn 0x%x +0x%x %s]"%(self.which,rva,self._symrows[i][0],rva-self._symrows[i][0],self._symrows[i][2])
        return "%s+0x%x"%(self.which,rva)
    def loc(self,rva):  # decode a Rust Location struct at rva
        d=self.rd(rva,24)
        if len(d)<24: return None
        fp,fl,ln,col=struct.unpack('<QQII',d)
        if fl<2 or fl>300 or fp<self.img or fp>self.img+0x4000000: return None
        s=self.rd(fp-self.img,fl)
        try: return (s.decode('latin1'),ln,col)
        except: return None
    def dis(self,rva,n=0x60,resolve=True):
        code=self.rd(rva,n); out=[]
        for ins in self.md.disasm(code,self.base+rva):
            a=ins.address-self.base; line="%8x  %s %s"%(a,ins.mnemonic,ins.op_str)
            if resolve and 'rip' in ins.op_str:
                try:
                    disp=int(ins.op_str.split('rip + ')[1].split(']')[0],16) if 'rip + ' in ins.op_str else -int(ins.op_str.split('rip - ')[1].split(']')[0],16)
                    tgt=a+ins.size+disp; loc=self.loc(tgt)
                    if loc: line+="   ; LOC %s:%d"%(loc[0].replace('\\','/').split('/')[-1],loc[1])
                    else: line+="   ; ->0x%x"%tgt
                except: pass
            if ins.mnemonic in('call','jmp') and ins.op_str.startswith('0x'):
                try: line+="   ; %s"%self.sym(int(ins.op_str,16)-self.base)
                except: pass
            out.append(line)
        return "\n".join(out)
    def iat_name(self,slot_rva):  # name of import at an IAT slot
        ir=self.imp[0]; d=self.off(ir)
        while True:
            oft,ts,fwd,name_rva,ft=struct.unpack('<IIIII',self.f[d:d+20])
            if name_rva==0 and ft==0: break
            dll=self.f[self.off(name_rva):self.f.index(b'\0',self.off(name_rva))].decode()
            ilt=oft if oft else ft; io=self.off(ilt); i=0
            while True:
                ent=struct.unpack('<Q',self.f[io+i*8:io+i*8+8])[0]
                if ent==0: break
                if ft+i*8==slot_rva:
                    nm='ORD%d'%(ent&0xffff) if ent>>63 else self.f[self.off((ent&0x7fffffff)+2):self.f.index(b'\0',self.off((ent&0x7fffffff)+2))].decode()
                    return dll+'!'+nm
                i+=1
            d+=20
        return None
if __name__=='__main__':
    import sys
    which=sys.argv[1]; rva=int(sys.argv[2],16); n=int(sys.argv[3],16) if len(sys.argv)>3 else 0x60
    print(PE(which).dis(rva,n))
