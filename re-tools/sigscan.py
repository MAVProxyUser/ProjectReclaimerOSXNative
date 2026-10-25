#!/usr/bin/env python3
"""Every match of a byte signature in a PE's executable sections (sig_parse / sig_find syntax: hex bytes, `??` = any).

    python3 re-tools/sigscan.py <file.exe|dll> "<sig>"

Unlike the loader's sig_find (unique match or nothing) this lists ALL hits, so you can see whether a new signature is
unique and which old one stopped matching. Port check for a new client: run a loosened form of a failed signature over the
old and the new build and line the hit lists up (the 0.9.23 watchdog: 13 hits in 0.9.22, 12 in 0.9.23, one gone).
"""
import sys, struct, re

path, sig = sys.argv[1], sys.argv[2]
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
pat = b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in sig.split())
for i in range(ns):
    vs, va, rs, rp = struct.unpack_from('<IIII', d, pe + 24 + so + i * 40 + 8); ch = struct.unpack_from('<I', d, pe + 24 + so + i * 40 + 36)[0]
    if not ch & 0x20000000:
        continue
    for m in re.finditer(pat, d[rp:rp + rs], re.S):
        print(hex(va + m.start()))
