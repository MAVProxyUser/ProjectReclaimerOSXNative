#!/usr/bin/env python3
"""PE RE helper for the Reclaimer binaries (no angr needed; pure-python + capstone-free).
Parses the PE section/.pdata tables to:
  func   <hexrva>            -> the .pdata function [start,end) containing an rva
  strref <text>              -> lea-rip refs to a literal string + the funcs they're in
  globref<hexrva>            -> lea/mov-rip refs to a data address + the funcs they're in
Addresses printed as exe+0xRVA. Pass the .exe/.dll path as $1. ImageBase assumed 0x140000000
for the exe and 0x180000000 for halo3.dll (override with --base).
"""
import sys, struct

def load(path):
    f = open(path, "rb").read()
    pe = f.find(b"PE\x00\x00"); coff = pe + 4
    nsec = struct.unpack("<H", f[coff+2:coff+4])[0]
    optsz = struct.unpack("<H", f[coff+16:coff+18])[0]
    opt = coff + 20
    imgbase = struct.unpack("<Q", f[opt+24:opt+32])[0]
    sectab = opt + optsz
    secs = []
    for i in range(nsec):
        e = sectab + i*40
        name = f[e:e+8].rstrip(b"\x00").decode("latin1")
        vsz, vrva, rsz, rptr = struct.unpack("<IIII", f[e+8:e+24])
        secs.append((name, vrva, max(vsz, rsz), rptr))
    return f, imgbase, secs

def rva2fo(secs, rva):
    for n, r, s, raw in secs:
        if r <= rva < r+s: return raw + (rva - r)
def fo2rva(secs, fo):
    for n, r, s, raw in secs:
        if raw <= fo < raw + s: return r + (fo - raw)

def pdata_funcs(f, secs):
    for n, r, s, raw in secs:
        if n == ".pdata":
            ents = []
            for p in range(raw, raw+s, 12):
                b, e, u = struct.unpack("<III", f[p:p+12])
                if b == 0 and e == 0: break
                ents.append((b, e))
            ents.sort(); return ents
    return []

def containing(ents, rva):
    import bisect
    starts = [x[0] for x in ents]
    i = bisect.bisect_right(starts, rva) - 1
    if i >= 0 and ents[i][0] <= rva < ents[i][1]: return ents[i]

def text_sec(secs):
    for n, r, s, raw in secs:
        if n == ".text": return r, s, raw

def lea_refs(f, secs, imgbase, target_vma):
    r, s, raw = text_sec(secs)
    text = f[raw:raw+s]; out = []
    for m in range(len(text)-7):
        if text[m] in (0x48, 0x4c) and text[m+1] == 0x8d and (text[m+2] & 0xc7) == 0x05:
            disp = struct.unpack("<i", text[m+3:m+7])[0]
            if imgbase + r + m + 7 + disp == target_vma:
                out.append(r + m)
    return out

def main():
    path = sys.argv[1]; cmd = sys.argv[2]
    base = 0x140000000
    if "--base" in sys.argv:
        base = int(sys.argv[sys.argv.index("--base")+1], 16)
    f, imgbase, secs = load(path)
    base = imgbase
    ents = pdata_funcs(f, secs)
    if cmd == "func":
        rva = int(sys.argv[3], 16)
        fn = containing(ents, rva)
        print("exe+0x%x -> FUNC exe+0x%x..0x%x (vma 0x%x, size 0x%x)" % (rva, fn[0], fn[1], base+fn[0], fn[1]-fn[0]) if fn else "no func")
    elif cmd == "strref":
        s = sys.argv[3].encode()
        i = f.find(s)
        if i < 0: print("string not found"); return
        rva = fo2rva(secs, i); print("string %r at exe+0x%x (vma 0x%x)" % (sys.argv[3], rva, base+rva))
        for lr in lea_refs(f, secs, base, base+rva):
            fn = containing(ents, lr)
            print("  lea exe+0x%x  in FUNC exe+0x%x..0x%x (vma 0x%x)" % (lr, fn[0], fn[1], base+fn[0]) if fn else "  lea exe+0x%x (no func)" % lr)
    elif cmd == "globref":
        g = int(sys.argv[3], 16)
        for lr in lea_refs(f, secs, base, base+g):
            fn = containing(ents, lr)
            print("  ref exe+0x%x  in FUNC exe+0x%x..0x%x (vma 0x%x)" % (lr, fn[0], fn[1], base+fn[0]) if fn else "  ref exe+0x%x (no func)" % lr)
    else:
        print(__doc__)

if __name__ == "__main__":
    main()
