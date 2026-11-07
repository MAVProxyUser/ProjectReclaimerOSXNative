#!/usr/bin/env python3
"""Halo 3 game results (halo3.dll, MCC build of 2026-10): dump the statistic definition tables the game results code
uses (static RE: game_results_statistic_increment halo3+0x123c?? reads min/max u16 at table+stat*24+0xc/+0xe; the
per-damage-type stats at 0x79316c). Prints each row's raw words and any string pointer it holds.
    python3 re-tools/h3results.py <halo3.dll> [table_rva count stride]"""
import sys, struct
sys.path.insert(0, __file__.rsplit('/', 1)[0])
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]; st = pe + 24 + so
secs = [struct.unpack_from('<IIII', d, st + i * 40 + 8) for i in range(ns)]
def fo(rva):
    for vs, va, rs, ra in secs:
        if va <= rva < va + max(vs, rs): return ra + rva - va
def cstr(va):
    if not (base <= va < base + 0x8000000): return None
    o = fo(va - base)
    if o is None: return None
    s = d[o:o + 64].split(b'\0')[0]
    return s.decode('latin1') if len(s) >= 2 and all(32 <= c < 127 for c in s) else None
tab = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x792c80
n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x40
stride = int(sys.argv[4], 0) if len(sys.argv) > 4 else 24
for i in range(n):
    o = fo(tab + i * stride); row = d[o:o + stride]
    q = [struct.unpack_from('<Q', row, k)[0] for k in range(0, stride - 7, 8)]
    names = [cstr(v) for v in q]
    hw = ' '.join(f'{struct.unpack_from("<H", row, k)[0]:04x}' for k in range(0, stride, 2))
    print(f'{i:3d} {hw}  ' + ' '.join(f'"{s}"' for s in names if s))
