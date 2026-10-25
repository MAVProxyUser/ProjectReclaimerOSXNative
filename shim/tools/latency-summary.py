#!/usr/bin/env python3
"""Medians of a client log's `[latency]` lines (latency.c: input -> halo3 present -> GPU done -> window -> screen) and of its
[fps] lines, stage by stage -- how much of the input -> screen time each part of the pipeline takes.
    python3 shim/tools/latency-summary.py <client log> [more logs ...]"""
import re, sys, statistics as st
for f in sys.argv[1:]:
    t = open(f, errors='replace').read()
    rows = [tuple(float(x) for x in m) for m in re.findall(r'\[latency\] [^\n]*?turned frame (-?\d+) ms -> GPU done (-?\d+) -> in the window (-?\d+) -> on screen ~(-?\d+) ms', t)]
    hz = re.findall(r'display (\d+) Hz', t); fr = re.findall(r'frame \+(\d+) after', t)
    fps = [float(x) for x in re.findall(r'\[perf\] ([\d.]+) fps', t)]
    print(f'== {f}: {len(rows)} measured inputs' + (f', display {hz[-1]} Hz' if hz else ''))
    if not rows: continue
    med = lambda i: st.median(r[i] for r in rows)
    p, g, w, s = med(0), med(1), med(2), med(3)
    print(f'  input -> halo3 presents the turned frame   {p:6.0f} ms   (the engine: input poll, tick, render thread)')
    print(f'         -> GPU done                         {g:6.0f} ms   (+{g-p:.0f}: GL submission thread + GL helper + GPU)')
    print(f'         -> in the window                    {w:6.0f} ms   (+{w-g:.0f}: the present thread picks it up and flushes)')
    print(f'         -> on screen (model)                {s:6.0f} ms   (+{s-w:.0f}: next refresh + the window server\'s composite)')
    print(f'  input -> screen: median {s:.0f}, 10% {sorted(r[3] for r in rows)[len(rows)//10]:.0f}, 90% {sorted(r[3] for r in rows)[len(rows)*9//10]:.0f} ms; '
          f'turned frame +{st.median(int(x) for x in fr):.0f} after the input' + (f'; fps median {st.median(fps):.1f} ({len(fps)} [perf] lines)' if fps else ''))
