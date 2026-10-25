#!/usr/bin/env python3
"""Shared PE helpers for the porting tools (patchscan.py, sigport.py). Read-only; nothing is executed.

    from pelib import Image
    im = Image('client/project-reclaimer-client-v0.9.23.exe')
    im.sig_hits('81 BD ?? ?? ?? ?? 00 CA 9A 3B 74 ??')      # every match (overlapping) in the executable sections
    im.func_of(0x87ace5)                                     # .pdata function (start, end) holding an RVA
    im.insns(0x87acdb, 0x87ad16)                             # capstone instructions (with operand detail)
    im.text_refs(b'still running after')                    # code that loads a literal (lea [rip+x] into the text)

    python3 re-tools/pelib.py <exe> [<rva> ...]              # the section table; each RVA's section and FILE offset

Addresses are RVAs (ImageBase 0x140000000 for the exe, 0x180000000 for halo3.dll). The image is mapped like the loader
maps it: sections at their RVAs, raw data, zero up to VirtualSize.
"""
import re, struct, bisect

try:
    import capstone
    from capstone import x86 as cx86
except ImportError:   # patchscan.py works without capstone; sigport.py needs it
    capstone = None


class Image:
    def __init__(self, path):
        f = open(path, 'rb').read(); self.path = path
        pe = struct.unpack_from('<I', f, 0x3c)[0]; ns = struct.unpack_from('<H', f, pe + 6)[0]; so = struct.unpack_from('<H', f, pe + 20)[0]
        opt = pe + 24; self.base = struct.unpack_from('<Q', f, opt + 24)[0]
        self.size = struct.unpack_from('<I', f, opt + 56)[0]; hdrs = struct.unpack_from('<I', f, opt + 60)[0]
        img = bytearray(self.size); img[:hdrs] = f[:hdrs]; self.secs = []; self.raw = []
        for i in range(ns):
            sh = opt + so + i * 40
            vs, va, rs, rp = struct.unpack_from('<IIII', f, sh + 8); ch = struct.unpack_from('<I', f, sh + 36)[0]
            n = min(rs, vs if vs else rs); img[va:va + n] = f[rp:rp + n]
            self.secs.append((va, vs, ch)); self.raw.append((f[sh:sh + 8].rstrip(b'\0').decode('latin-1'), va, vs, rp, rs))
        self.b = bytes(img)
        self.exec = [(va, vs) for va, vs, ch in self.secs if ch & 0x20000000]
        self.data = [(va, vs) for va, vs, ch in self.secs if not ch & 0x20000000]
        pd_rva, pd_sz = struct.unpack_from('<II', f, opt + 112 + 3 * 8)        # data directory 3: exception (.pdata)
        self.funcs = sorted(struct.unpack_from('<II', self.b, pd_rva + i * 12) for i in range(pd_sz // 12)) if pd_rva else []
        self._starts = [s for s, e in self.funcs]
        self._md = None

    # -- bytes ------------------------------------------------------------------------------------------------------
    def file_offset(self, rva):
        """the file offset holding an RVA (None: not in any section's raw data, e.g. zero-filled .bss)"""
        for name, va, vs, rp, rs in self.raw:
            if va <= rva < va + min(vs or rs, rs): return rva - va + rp
        return None

    def i32(self, a): return struct.unpack_from('<i', self.b, a)[0]
    def u32(self, a): return struct.unpack_from('<I', self.b, a)[0]
    def is_exec(self, a): return any(va <= a < va + vs for va, vs in self.exec)

    # -- signatures (sig_parse / sig_find semantics) ------------------------------------------------------------------
    @staticmethod
    def sig_regex(sig):
        toks = sig.split()
        if not toks or toks[0] == '??': raise ValueError('a signature must start with a literal byte (sig_find): ' + sig)
        return re.compile(b'(?=(' + b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in toks) + b'))', re.S)

    def sig_hits(self, sig):
        pat = self.sig_regex(sig); out = []
        for va, vs in self.exec: out += [m.start() for m in pat.finditer(self.b, va, va + vs)]   # (absolute positions)
        return out

    def find_unique(self, sig):
        h = self.sig_hits(sig)
        return (h[0] if len(h) == 1 else None), len(h)

    # -- functions ----------------------------------------------------------------------------------------------------
    def func_of(self, rva):
        i = bisect.bisect_right(self._starts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]: return self.funcs[i]
        return None

    # -- disassembly --------------------------------------------------------------------------------------------------
    def md(self):
        if self._md is None:
            if capstone is None: raise SystemExit('capstone is needed: python3 -m pip install --user capstone')
            self._md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); self._md.detail = True
        return self._md

    def insns(self, a, b):
        return list(self.md().disasm(self.b[a:b], a))

    # -- references ---------------------------------------------------------------------------------------------------
    def find_text(self, text):
        out = []
        for va, vs in self.data:
            i = self.b.find(text, va, va + vs)
            while i >= 0:
                out.append(i); i = self.b.find(text, i + 1, va + vs)
        return out

    def rip_refs(self, lo, hi):
        """(site, target) of every instruction with a RIP-relative operand into [lo, hi): every disp32 in the code that lands
        in the window, confirmed by decoding the instruction it belongs to (re-tools/ripref.py's method)"""
        import numpy as np
        md = self.md(); out = []
        for va, vs in self.exec:
            arr = np.frombuffer(self.b, dtype=np.uint8, count=vs, offset=va); n = vs - 4
            if n <= 0: continue
            d = (arr[0:n].astype(np.int64) | (arr[1:n + 1].astype(np.int64) << 8) | (arr[2:n + 2].astype(np.int64) << 16) |
                 (arr[3:n + 3].astype(np.int64) << 24))
            d = np.where(d >= 2 ** 31, d - 2 ** 32, d); tt = va + np.arange(n) + 4 + d
            for j in np.nonzero((tt >= lo - 8) & (tt < hi))[0].tolist():
                # the instruction holding this disp32: the LONGEST decode whose displacement is exactly here (a shorter
                # one can start inside its prefixes: `0F B0 0D` inside `F0 0F B0 0D` = cmpxchg without the lock)
                for back in (8, 7, 6, 5, 4, 3, 2):
                    st = va + j - back
                    if st < va: continue
                    ins = next(md.disasm(self.b[st:st + 16], st, 1), None)
                    if not ins or ins.disp_size != 4 or ins.address + ins.disp_offset != va + j: continue
                    t = None
                    for op in ins.operands:
                        if op.type == cx86.X86_OP_MEM and op.mem.base == cx86.X86_REG_RIP: t = ins.address + ins.size + op.mem.disp
                    if t is not None and lo <= t < hi: out.append((ins.address, t)); break
        return out

    def callers_of(self, target):
        """sites of `call rel32` (E8) to target, from an index of every E8 in the code whose target is a .pdata function
        start (built once per image with numpy; a stray E8 byte rarely lands exactly on a function start)"""
        if not hasattr(self, '_calls'):
            import numpy as np
            starts = np.array(self._starts, dtype=np.int64); idx = {}
            for va, vs in self.exec:
                arr = np.frombuffer(self.b, dtype=np.uint8, count=vs, offset=va); pos = np.nonzero(arr[:vs - 5] == 0xE8)[0]
                d = (arr[pos + 1].astype(np.int64) | (arr[pos + 2].astype(np.int64) << 8) | (arr[pos + 3].astype(np.int64) << 16) |
                     (arr[pos + 4].astype(np.int64) << 24))
                d = np.where(d >= 2 ** 31, d - 2 ** 32, d); tt = va + pos + 5 + d
                ok = np.isin(tt, starts)
                for site, t in zip((va + pos[ok]).tolist(), tt[ok].tolist()): idx.setdefault(t, []).append(site)
            self._calls = idx
        return self._calls.get(target, [])

    def callees(self, f):
        """direct call targets (decoded) inside function f = (start, end)"""
        return [x.operands[0].imm for x in self.insns(f[0], f[1])
                if x.mnemonic == 'call' and x.operands and x.operands[0].type == cx86.X86_OP_IMM]

    def text_refs_many(self, texts):
        """{text: code sites loading it} for many texts in one pass over the code (cached per image)"""
        import numpy as np
        if not hasattr(self, '_tref'): self._tref = {}
        todo = [t for t in set(texts) if t not in self._tref]
        if todo:
            wins = sorted((s - 64, s + 1, t) for t in todo for s in self.find_text(t))   # (Rust concatenates literals)
            res = {t: [] for t in todo}
            if wins:
                lo = np.array([w[0] for w in wins], dtype=np.int64); md = self.md()
                for va, vs in self.exec:
                    arr = np.frombuffer(self.b, dtype=np.uint8, count=vs, offset=va); n = vs - 4
                    d = (arr[0:n].astype(np.int64) | (arr[1:n + 1].astype(np.int64) << 8) | (arr[2:n + 2].astype(np.int64) << 16) |
                         (arr[3:n + 3].astype(np.int64) << 24))
                    d = np.where(d >= 2 ** 31, d - 2 ** 32, d); tt = va + np.arange(n) + 4 + d
                    k = np.searchsorted(lo, tt, side='right') - 1
                    near = np.nonzero((k >= 0) & (tt < lo[np.maximum(k, 0)] + 65))[0]   # inside the window that starts last before it
                    for j in near.tolist():
                        t = int(tt[j]); kk = int(k[j]); hit = [w for w in wins[max(0, kk - 8):kk + 1] if w[0] <= t < w[1]]
                        if not hit: continue
                        for back in (8, 7, 6, 5, 4, 3, 2):
                            st = va + j - back
                            if st < va: continue
                            ins = next(md.disasm(self.b[st:st + 16], st, 1), None)
                            if not ins or ins.disp_size != 4 or ins.address + ins.disp_offset != va + j: continue
                            if any(op.type == cx86.X86_OP_MEM and op.mem.base == cx86.X86_REG_RIP for op in ins.operands):
                                for w in hit: res[w[2]].append(ins.address)
                            break
            self._tref.update({t: sorted(set(v)) for t, v in res.items()})
        return {t: self._tref[t] for t in texts}

    def text_refs(self, text):
        """code that loads `text` (or a literal block that starts a few bytes before it -- Rust concatenates literals)"""
        out = []
        for s in self.find_text(text):
            out += [site for site, t in self.rip_refs(s - 64, s + 1) if t <= s]
        return out


if __name__ == '__main__':
    import sys
    im = Image(sys.argv[1])
    print(f'image base {im.base:#x}, size {im.size:#x}')
    for name, va, vs, rp, rs in im.raw: print(f'  {name:8s} rva {va:#9x} .. {va + vs:#9x}   file {rp:#9x} .. {rp + rs:#9x}')
    for a in sys.argv[2:]:
        r = int(a, 16); o = im.file_offset(r); f = im.func_of(r)
        print(f'exe+{r:#x}: file offset ' + (f'{o:#x}' if o is not None else 'none (not in the file)') + (f', in function {f[0]:#x}' if f else ''))
