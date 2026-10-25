#!/usr/bin/env python3
"""Per A/B phase (PELOAD_AB=<feature>:<secs>): fps from the [ab] line and the [submit] profile's waits (PELOAD_D3DGL_PROF=1) of
the lines that fell in that phase -- each `[ab] <feature> on|off:` line closes the phase it names.
    python3 shim/tools/ab-phases.py <client log>"""
import re, sys, statistics as st
t = open(sys.argv[1], errors='replace').read()
cur = []; out = []
for m in re.finditer(r'\[submit\] \d+ commands[^\n]*?game thread waited (\d+) ms[^\n]*?for compute (\d+) ms|\[ab\] (\w+) (on |off): ([\d.]+) fps, (\d+) draws', t):
    if m.group(1) is not None: cur.append((int(m.group(1)), int(m.group(2)))); continue
    out.append((m.group(3), m.group(4).strip(), float(m.group(5)), int(m.group(6)), cur)); cur = []
for feat, ph, fps, dr, w in out:
    gw = st.mean(x[0] for x in w) if w else -1; cw = st.mean(x[1] for x in w) if w else -1
    print(f'{feat} {ph:3}  {fps:5.1f} fps  {dr:5d} draws/frame | per [submit] line: render thread waited {gw:6.0f} ms, GL thread waited for compute {cw:5.0f} ms ({len(w)} lines)')
