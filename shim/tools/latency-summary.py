#!/usr/bin/env python3
"""Medians of a client log's `[latency]` lines (latency.c: input -> halo3 present -> GPU done -> window -> screen) and of its
[perf] lines, stage by stage -- how much of the input -> screen time each part of the pipeline takes.
    python3 shim/tools/latency-summary.py <client log> [more logs ...]
With PELOAD_AB=<feature>:<secs> in the log, the measurements are also split by the A/B phase they fell in (each `[ab] <feature>
on|off:` line closes a phase) -- one match, both settings."""
import re, sys, statistics as st
LAT = re.compile(r'\[latency\] [^\n]*?turned frame (-?\d+) ms -> GPU done (-?\d+) -> in the window (-?\d+) -> on screen ~(-?\d+) ms')
def report(name, rows, fps):
    if not rows: print(f'  {name}: no measured inputs'); return
    med = lambda i: st.median(r[i] for r in rows)
    p, g, w, s = med(0), med(1), med(2), med(3); srt = sorted(r[3] for r in rows)
    print(f'  {name}: {len(rows)} inputs' + (f', {st.median(fps):.1f} fps' if fps else ''))
    print(f'    input -> halo3 presents the turned frame   {p:6.0f} ms   (the engine: input poll, tick, render thread)')
    print(f'           -> GPU done                         {g:6.0f} ms   (+{g-p:.0f}: GL submission + GL helper + GPU)')
    print(f'           -> in the window                    {w:6.0f} ms   (+{w-g:.0f}: the present thread picks it up and flushes)')
    print(f'           -> on screen (model)                {s:6.0f} ms   (+{s-w:.0f}: next refresh + the window server\'s composite)')
    print(f'    input -> screen: median {s:.0f}, 10% {srt[len(srt)//10]:.0f}, 90% {srt[len(srt)*9//10]:.0f} ms')
for f in sys.argv[1:]:
    t = open(f, errors='replace').read(); hz = re.findall(r'display (\d+) Hz', t)
    print(f'== {f}' + (f' (display {hz[-1]} Hz)' if hz else ''))
    rows, fps, phases, cur, curfps = [], [], {}, [], []
    for line in re.split(r'\n|(?=  \[)', t):
        m = LAT.search(line)
        if m: r = tuple(float(x) for x in m.groups()); rows.append(r); cur.append(r); continue
        m = re.search(r'\[perf\] ([\d.]+) fps', line)
        if m: fps.append(float(m.group(1))); curfps.append(float(m.group(1))); continue
        m = re.search(r'\[ab\] (\w+) (on |off):', line)
        if m: k = f'{m.group(1)} {m.group(2).strip()}'; phases.setdefault(k, ([], [])); phases[k][0].extend(cur); phases[k][1].extend(curfps); cur, curfps = [], []
    report('all', rows, fps)
    for k, (r, fp) in sorted(phases.items()): report(k, r, fp)
