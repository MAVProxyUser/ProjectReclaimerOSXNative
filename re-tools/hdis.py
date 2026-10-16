#!/usr/bin/env python3
"""Disassemble RVA ranges of a PE (exe or DLL) with capstone, annotating RIP-relative operands with the
import name they reference (IAT slots) or the target RVA.

    python3 re-tools/hdis.py <file.exe|dll> <start>-<end> [<start>-<end> ...]      (hex RVAs)

ImageBase is read from the PE header (0x140000000 for the Reclaimer exe, 0x180000000 for halo3.dll).
Needs capstone (system python3 has it here; or the re-tools venv)."""
import capstone, struct, sys, re

path = sys.argv[1]
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
ns = struct.unpack_from('<H', d, pe + 6)[0]
so = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
st = pe + 24 + so
secs = []
for i in range(ns):
    vs, va, rs, ra = struct.unpack_from('<IIII', d, st + i * 40 + 8)
    secs.append((va, vs, ra, rs))

def fo(rva):
    for va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ra + rva - va
    return None

iat = {}
imp_rva = struct.unpack_from('<I', d, pe + 24 + 120)[0]
p = fo(imp_rva) if imp_rva else None
while p:
    oft, ts, fc, name, ft = struct.unpack_from('<IIIII', d, p)
    if not name:
        break
    t = fo(oft or ft); k = 0
    while True:
        v = struct.unpack_from('<Q', d, t + k * 8)[0]
        if not v:
            break
        iat[ft + k * 8] = ('#%d' % (v & 0xffff)) if v >> 63 else d[fo(v & 0x7fffffff) + 2:fo(v & 0x7fffffff) + 80].split(b'\0')[0].decode()
        k += 1
    p += 20

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for rng in sys.argv[2:]:
    s, e = [int(x, 16) for x in rng.split('-')]
    print('----', hex(s))
    for x in md.disasm(d[fo(s):fo(e)], base + s):
        ex = ''
        m = re.search(r'\[rip \+ 0x([0-9a-f]+)\]', x.op_str) or re.search(r'\[rip - 0x([0-9a-f]+)\]', x.op_str)
        if m:
            off = int(m.group(1), 16) * (-1 if '- 0x' in x.op_str else 1)
            tgt = x.address + x.size + off - base
            ex = iat.get(tgt, '') or hex(tgt)
        print(hex(x.address - base), x.mnemonic, x.op_str, ex)
