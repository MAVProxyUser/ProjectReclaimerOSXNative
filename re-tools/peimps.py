#!/usr/bin/env python3
"""A PE's imports, one `dll!name` (or `dll!#ordinal`) per line.

    python3 re-tools/peimps.py <file.exe|dll>
    diff <(python3 re-tools/peimps.py old.exe | sort) <(python3 re-tools/peimps.py new.exe | sort)

A new client release that imports something the loader doesn't implement gets a logging stub returning 0 -- S_OK for COM /
Media Foundation calls with nothing behind it (0.9.22's MFStartup & co.). Diff the import lists of every new build.
"""
import sys, struct

d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<IIII', d, pe + 24 + so + i * 40 + 8) for i in range(ns)]


def off(rva):
    for vs, va, rs, rp in secs:
        if va <= rva < va + max(vs, rs):
            return rp + rva - va


o = off(struct.unpack_from('<I', d, pe + 24 + 120)[0])
while True:
    ilt, ts, fc, name, iat = struct.unpack_from('<IIIII', d, o); o += 20
    if not name:
        break
    dll = d[off(name):].split(b'\0')[0].decode().lower(); t = off(ilt or iat)
    while True:
        v = struct.unpack_from('<Q', d, t)[0]; t += 8
        if not v:
            break
        nm = '#%d' % (v & 0xffff) if v >> 63 else d[off(v & 0x7fffffff) + 2:].split(b'\0')[0].decode()
        print(dll + '!' + nm)
