#!/usr/bin/env python3
"""List halo3.dll HaloScript function-table entries: name -> the code pointers stored next to the name pointer.
    python3 re-tools/hsfuncs.py <halo3.dll> [name ...]
Finds each name string, then 8-byte pointers to it in the image, and prints the qwords around that pointer
(the hs_function_definition entry) marking the ones that point into executable sections."""
import struct, sys
path = sys.argv[1]; names = sys.argv[2:]
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]; st = pe + 24 + so
secs = []
for i in range(ns):
    nm = d[st + i*40: st + i*40 + 8].rstrip(b'\0').decode(); vs, va, rs, ra = struct.unpack_from('<IIII', d, st + i*40 + 8); ch = struct.unpack_from('<I', d, st + i*40 + 36)[0]
    secs.append((nm, va, max(vs, rs), ra, rs, ch))
def rva2off(r):
    for nm, va, sz, ra, rs, ch in secs:
        if va <= r < va + rs: return ra + r - va
def off2rva(o):
    for nm, va, sz, ra, rs, ch in secs:
        if ra <= o < ra + rs: return va + o - ra
def is_code(v):
    r = v - base
    return any(va <= r < va + sz and ch & 0x20000000 for nm, va, sz, ra, rs, ch in secs)
for name in names:
    pat = b'\0' + name.encode() + b'\0'; i = d.find(pat)
    while i >= 0:
        sva = base + off2rva(i + 1); ptr = struct.pack('<Q', sva); j = d.find(ptr)
        while j >= 0:
            if j % 8 == 0:
                q = [struct.unpack_from('<Q', d, j + k*8)[0] for k in range(-2, 8)]
                desc = ' '.join(('C:%x' % (v - base)) if is_code(v) else ('%x' % v) for v in q)
                print('%-34s entry@%x: %s' % (name, off2rva(j), desc))
            j = d.find(ptr, j + 1)
        i = d.find(pat, i + 1)
