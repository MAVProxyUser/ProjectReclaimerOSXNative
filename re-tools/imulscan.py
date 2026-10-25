#!/usr/bin/env python3
"""Every `imul r64, [rip+d], imm8` with a given element size, grouped by the global it reads (most common first).

    python3 re-tools/imulscan.py <file.exe|dll> <element size, hex: 68>

`len * size` against a global = a Vec's length field with that element size: how stats.c finds the moderation table's
players Vec (0x68-byte entries) without a fixed offset (0.9.22 moved it to lock+0x170).
"""
import sys, struct, re, collections

path, imm = sys.argv[1], int(sys.argv[2], 16)
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
c = collections.Counter(); ex = {}
for i in range(ns):
    sh = pe + 24 + so + i * 40; vs, va, rs, rp = struct.unpack_from('<IIII', d, sh + 8); ch = struct.unpack_from('<I', d, sh + 36)[0]
    if not ch & 0x20000000:
        continue
    data = d[rp:rp + rs]
    for m in re.finditer(rb'[\x48\x4c]\x6b[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', data):   # REX.W 6B /r, modrm = [rip+disp32]
        j = m.start()
        if j + 8 > len(data) or data[j + 7] != imm:
            continue
        t = va + j + 8 + struct.unpack_from('<i', data, j + 3)[0]
        c[t] += 1; ex.setdefault(t, va + j)
for t, n in c.most_common(10):
    print(hex(t), n, 'e.g. at', hex(ex[t]))
