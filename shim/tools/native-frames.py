#!/usr/bin/env python3
"""Map host code addresses (a native-app crash's "frames:" line) to the halo3 functions the app lifted.

    NHOOK_CODETAB_DUMP=<file> bash shim/native.sh ...      (nhook writes the code table: "halo3 rva, host entry" per line)
    python3 shim/tools/native-frames.py <file> <log or addresses...>

Each address is shown as halo3+<rva of the nearest lifted entry at or below it> + offset; a log file argument takes the
addresses from its "[runtime] host thread ... frames:" lines. Addresses below the first entry / far past the last are the
app's own code (no halo3 name)."""
import bisect, re, sys
tab = []
for ln in open(sys.argv[1]):
    r, h = ln.split(); tab.append((int(h, 16), int(r, 16)))
tab.sort(); hs = [h for h, _ in tab]
addrs = []
for a in sys.argv[2:]:
    if re.fullmatch(r'(0x)?[0-9a-fA-F]+', a): addrs.append(int(a, 16)); continue
    for ln in open(a, errors='replace'):
        if 'frames:' in ln: addrs += [int(x, 16) for x in re.findall(r'0x[0-9a-f]+', ln.split('frames:', 1)[1])]
for a in addrs:
    i = bisect.bisect_right(hs, a) - 1
    if i < 0 or a - hs[i] > 0x40000: print(f"0x{a:x}  (not lifted halo3 code)"); continue
    print(f"0x{a:x}  halo3+0x{tab[i][1]:x} + 0x{a - hs[i]:x}")
