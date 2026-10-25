#!/usr/bin/env python3
"""Every RIP-relative reference into an address window of a PE, grouped by offset inside the window.

    python3 re-tools/ripref.py <file.exe|dll> <lo> <hi> [n]      (hex RVAs; n = sample sites shown per offset, default 2)

Prints `<offset from lo> <count> [(site rva, mnemonic, operands), ...]`. Finds who reads / writes a global or a struct
in .data (count per field = a layout fingerprint you can compare between builds: 0.9.22's moderation table showed its
inserted fields this way) and which code loads a string (pass the string's RVA window; Rust concatenates literals, so
look at the offsets that ARE referenced). RVAs, not file offsets: convert a string's file offset first (refs.py strref
prints RVAs). Needs capstone + numpy (python3 -m pip install --user capstone numpy).
"""
import sys, re, struct, collections
import capstone, numpy as np

path, lo, hi = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
nshow = int(sys.argv[4]) if len(sys.argv) > 4 else 2
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
cnt = collections.Counter(); sites = collections.defaultdict(list)
for i in range(ns):
    sh = pe + 24 + so + i * 40; vs, va, rs, rp = struct.unpack_from('<IIII', d, sh + 8); ch = struct.unpack_from('<I', d, sh + 36)[0]
    if not ch & 0x20000000:            # IMAGE_SCN_MEM_EXECUTE
        continue
    data = d[rp:rp + rs]; arr = np.frombuffer(data, dtype=np.uint8); n = len(data) - 4
    disp = (arr[0:n].astype(np.int64) | (arr[1:n + 1].astype(np.int64) << 8) | (arr[2:n + 2].astype(np.int64) << 16) |
            (arr[3:n + 3].astype(np.int64) << 24))
    disp = np.where(disp >= 2 ** 31, disp - 2 ** 32, disp)
    tgt = va + np.arange(n) + 4 + disp          # a disp32 at j points (roughly) here; the exact end of the instruction decides
    for j in np.nonzero((tgt >= lo - 4) & (tgt < hi))[0].tolist():
        for back in (3, 2, 4, 5, 6, 7, 8):      # decode from where the instruction holding this disp32 may start
            st = j - back
            if st < 0:
                continue
            ins = next(md.disasm(data[st:st + 16], base + va + st, 1), None)
            if not ins or not (ins.address <= base + va + j and ins.address + ins.size >= base + va + j + 4):
                continue
            m = re.search(r'rip ([+-]) (0x[0-9a-f]+)', ins.op_str)
            if not m:
                continue
            t = ins.address + ins.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1) - base
            if lo <= t < hi:
                cnt[t - lo] += 1; sites[t - lo].append((hex(ins.address - base), ins.mnemonic, ins.op_str)); break
for k in sorted(cnt):
    print(hex(k), cnt[k], sites[k][:nshow])
