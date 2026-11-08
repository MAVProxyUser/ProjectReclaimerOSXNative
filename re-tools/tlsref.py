#!/usr/bin/env python3
"""Who uses a halo3 TLS block field. halo3's idiom: `mov rax, gs:[0x58]` ; `mov edx, FIELD` ; `mov rax, [rax + rcx*8]` (the
block) ; `mov rX, [rdx + rax]` -- also the plain `mov rX, [rB + FIELD]` within --back instructions of a gs:[0x58] load. With
--then, only the hits whose loaded register is then used at one of the given offsets within --ahead instructions. Prints the function (.pdata) of each hit. Example: the users of TLS +0xC0 that touch its +0xD4 / +0xD8:
    python3 re-tools/tlsref.py <halo3.dll> 0xc0 --then 0xd4,0xd8
Needs capstone (re-tools/angrenv/bin/python has it)."""
import sys, struct, re, bisect, capstone

def sections(d):
    pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]; out = {}
    for i in range(ns):
        sh = pe + 24 + so + i * 40; nm = d[sh:sh + 8].rstrip(b'\0').decode(); vs, va, rs, rp = struct.unpack_from('<IIII', d, sh + 8)
        out[nm] = (va, vs, rp, rs)
    return base, out

def main():
    a = sys.argv[1:]
    if len(a) < 2: print(__doc__); sys.exit(1)
    path, field = a[0], int(a[1], 16)
    then = [int(x, 16) for x in a[a.index('--then') + 1].split(',')] if '--then' in a else []
    back = int(a[a.index('--back') + 1]) if '--back' in a else 24
    ahead = int(a[a.index('--ahead') + 1]) if '--ahead' in a else 60
    d = open(path, 'rb').read(); base, secs = sections(d)
    fstarts, fends = [], []
    if '.pdata' in secs:
        va, vs, rp, rs = secs['.pdata']
        for i in range(0, min(vs, rs) // 12 * 12, 12):
            b, e, _ = struct.unpack_from('<III', d, rp + i)
            if b: fstarts.append(b); fends.append(e)
        o = sorted(range(len(fstarts)), key=lambda k: fstarts[k]); fstarts = [fstarts[k] for k in o]; fends = [fends[k] for k in o]
    def func(rva):
        k = bisect.bisect_right(fstarts, rva) - 1
        return (fstarts[k], fends[k]) if k >= 0 and fstarts[k] <= rva < fends[k] else None
    va, vs, rp, rs = secs['.text']
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.skipdata = True
    ins = list(md.disasm(d[rp:rp + min(vs, rs)], va))
    pat = re.compile(r'^(\w+), qword ptr \[(\w+) \+ %s\]$' % hex(field))
    pair = re.compile(r'^(\w+), qword ptr \[(\w+) \+ (\w+)\]$')
    imm = re.compile(r'^(\w+), %s$' % hex(field))
    R32 = {'eax': 'rax', 'ebx': 'rbx', 'ecx': 'rcx', 'edx': 'rdx', 'esi': 'rsi', 'edi': 'rdi', 'ebp': 'rbp', 'esp': 'rsp'}
    def r64(r): return R32.get(r, r[:-1] if r.startswith('r') and r.endswith('d') else r)
    seen = set()
    for i, x in enumerate(ins):
        if x.mnemonic != 'mov': continue
        m = pat.match(x.op_str)
        if m:
            if not any('gs:[0x58]' in y.op_str for y in ins[max(0, i - back):i]): continue
        else:
            m = pair.match(x.op_str)
            if not m: continue
            prev = ins[max(0, i - back):i]
            if not any('gs:[0x58]' in y.op_str for y in prev): continue
            regs = {r64(y.op_str.split(',')[0]) for y in prev if y.mnemonic == 'mov' and imm.match(y.op_str)}
            if m.group(2) not in regs and m.group(3) not in regs: continue
        reg = m.group(1); uses = []
        if then:
            for y in ins[i + 1:i + 1 + ahead]:
                if any(re.search(r'\[%s \+ %s\]' % (reg, hex(o)), y.op_str) for o in then): uses.append(y)
                if re.match(r'^%s,' % reg, y.op_str) and y.mnemonic in ('mov', 'lea', 'xor', 'pop'): break   # reg overwritten
            if not uses: continue
        f = func(x.address)
        key = (f, x.address)
        if key in seen: continue
        seen.add(key)
        print('halo3+0x%x  %s %s   (function 0x%x..0x%x)' % (x.address, x.mnemonic, x.op_str, *(f or (0, 0))))
        for y in uses: print('      halo3+0x%x  %s %s' % (y.address, y.mnemonic, y.op_str))

if __name__ == '__main__':
    main()
