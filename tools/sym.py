#!/usr/bin/env python3
# Resolve exe RVAs to function+source via the symmap. Usage: sym.py <rva_hex> [rva_hex ...]
import sys
rows=[]
for line in open('analysis/reclaimer-exe-symmap.tsv'):
    if line.startswith('#'): continue
    p=line.rstrip('\n').split('\t')
    if len(p)>=4: rows.append((int(p[0],16),int(p[1],16),p[3]))
rows.sort()
import bisect
starts=[r[0] for r in rows]
for a in sys.argv[1:]:
    r=int(a,16); i=bisect.bisect_right(starts,r)-1
    if i>=0 and rows[i][0]<=r<rows[i][1]:
        print("exe+0x%-9x fn exe+0x%-8x (+0x%-5x) %s"%(r,rows[i][0],r-rows[i][0],rows[i][2]))
    else:
        print("exe+0x%-9x (no function / thunk)"%r)
