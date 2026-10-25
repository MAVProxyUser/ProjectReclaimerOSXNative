#!/usr/bin/env python3
"""What a function calls and which strings it loads: imports by name (IAT slots), direct callees, and `lea [rip+x]` targets
that start with printable text.

    python3 re-tools/fcalls.py <file.exe|dll> <start>-<end> [<start>-<end> ...]      (hex RVAs, e.g. from refs.py func)

A quick way to tell what an unknown Rust function is for (its error strings, the Win32 calls it makes) before decompiling it.
"""
import sys, struct
import capstone

path = sys.argv[1]; ranges = [tuple(int(x, 16) for x in r.split('-')) for r in sys.argv[2:]]
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<IIII', d, pe + 24 + so + i * 40 + 8) for i in range(ns)]


def off(r):
    for vs, va, rs, rp in secs:
        if va <= r < va + max(vs, rs):
            return rp + r - va


iat = {}
o = off(struct.unpack_from('<I', d, pe + 24 + 120)[0])
while True:
    ilt, ts, fc, name, ia = struct.unpack_from('<IIIII', d, o); o += 20
    if not name:
        break
    dll = d[off(name):].split(b'\0')[0].decode(); t = off(ilt or ia); k = 0
    while True:
        v = struct.unpack_from('<Q', d, t + 8 * k)[0]
        if not v:
            break
        iat[ia + 8 * k] = dll + '!' + ('#%d' % (v & 0xffff) if v >> 63 else d[off(v & 0x7fffffff) + 2:].split(b'\0')[0].decode())
        k += 1
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for a, b in ranges:
    print('== FUNC %x..%x' % (a, b))
    for ins in md.disasm(d[off(a):off(a) + (b - a)], a):
        if ins.mnemonic == 'call':
            if ins.op_str.startswith('qword ptr [rip'):
                disp = int(ins.op_str.split('+ ')[1].rstrip(']'), 16) if '+ ' in ins.op_str else 0
                print('   imp', iat.get(ins.address + ins.size + disp, '?'))
            elif ins.op_str.startswith('0x'):
                print('   fn ', ins.op_str)
        elif ins.mnemonic == 'lea' and 'rip' in ins.op_str:
            disp = ins.op_str.split('rip ')[1].rstrip(']'); t = ins.address + ins.size + (1 if disp[0] == '+' else -1) * int(disp[2:], 16)
            o = off(t); s = d[o:o + 48] if o else b''
            if s and all(32 <= c < 127 for c in s[:6]):
                print('   str', s.split(b'\0')[0][:48].decode('latin1'))
