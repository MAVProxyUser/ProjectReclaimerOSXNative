#!/usr/bin/env python3
"""Direct `call rel32` sites of one or more functions, with the .pdata function each call sits in.

    python3 re-tools/callers.py <file.exe|dll> <target rva> [<target rva> ...]     (hex)

Only direct E8 calls: a function reached through a pointer (a detour the client installs, a closure, a vtable) shows no
callers here -- look for where its address is loaded instead (ripref.py over a one-byte window at its start).
Needs numpy.
"""
import sys, struct, bisect
import numpy as np

path = sys.argv[1]; targets = [int(x, 16) for x in sys.argv[2:]]
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<IIII', d, pe + 24 + so + i * 40 + 8) + (struct.unpack_from('<I', d, pe + 24 + so + i * 40 + 36)[0],) for i in range(ns)]


def off(r):
    for vs, va, rs, rp, ch in secs:
        if va <= r < va + vs:
            return rp + r - va


pd_rva, pd_sz = struct.unpack_from('<II', d, pe + 24 + 112 + 3 * 8)       # data directory 3 = exception (.pdata)
o = off(pd_rva); funcs = [struct.unpack_from('<II', d, o + i * 12) for i in range(pd_sz // 12)]
starts = [f[0] for f in funcs]


def fn(r):
    i = bisect.bisect_right(starts, r) - 1
    return funcs[i] if i >= 0 and funcs[i][0] <= r < funcs[i][1] else None


for vs, va, rs, rp, ch in secs:
    if not ch & 0x20000000:
        continue
    a = np.frombuffer(d[rp:rp + rs], dtype=np.uint8); idx = np.nonzero(a[:-5] == 0xE8)[0]
    rel = (a[idx + 1].astype(np.int64) | (a[idx + 2].astype(np.int64) << 8) | (a[idx + 3].astype(np.int64) << 16) |
           (a[idx + 4].astype(np.int64) << 24))
    rel = np.where(rel >= 2 ** 31, rel - 2 ** 32, rel); tgt = va + idx + 5 + rel
    for t in targets:
        for j in idx[tgt == t].tolist():
            f = fn(va + j)
            print(f'call {t:#x} from {va + j:#x}' + (f'  in FUNC {f[0]:#x}..{f[1]:#x}' if f else ''))
