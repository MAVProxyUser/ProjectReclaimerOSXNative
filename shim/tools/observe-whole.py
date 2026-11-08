#!/usr/bin/env python3
"""How many WHOLE matches a match-recorder.py log shows (shim/tools/observe-public.sh; 2026-11-08 15:00): a finished recording of
1500+ samples counts when it is not the visit's first, or when the first began at its match's start (the "(match time N s)" the
recorder prints, N < 90) -- a join right as a match begins used to be thrown away as "maybe mid-match".
    python3 shim/tools/observe-whole.py <recorder log>     -> prints the count"""
import re, sys
n = 0; first = True; start_ok = False
try: lines = open(sys.argv[1], errors='replace').read().splitlines()
except OSError: lines = []
for l in lines:
    if '[rec] recording ' in l:
        m = re.search(r'\(match time (\d+) s\)', l); start_ok = bool(m and int(m.group(1)) < 90); continue
    m = re.search(r': (\d+) samples', l)
    if m:
        if int(m.group(1)) >= 1500 and (not first or start_ok): n += 1
        first = False
print(n)
